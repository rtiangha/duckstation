#include "spu.h"
#include "YBaseLib/Log.h"
#include "common/audio_stream.h"
#include "common/state_wrapper.h"
#include "dma.h"
#include "host_interface.h"
#include "interrupt_controller.h"
#include "system.h"
#include <imgui.h>
Log_SetChannel(SPU);

SPU::SPU() = default;

SPU::~SPU() = default;

bool SPU::Initialize(System* system, DMA* dma, InterruptController* interrupt_controller)
{
  m_audio_stream = system->GetHostInterface()->GetAudioStream();
  m_system = system;
  m_dma = dma;
  m_interrupt_controller = interrupt_controller;
  return true;
}

void SPU::Reset()
{
  m_SPUCNT.bits = 0;
  m_SPUSTAT.bits = 0;
  m_transfer_address = 0;
  m_transfer_address_reg = 0;
  m_irq_address = 0;
  m_main_volume_left.bits = 0;
  m_main_volume_right.bits = 0;
  m_key_on_register = 0;
  m_key_off_register = 0;
  m_endx_register = 0;
  m_reverb_on_register = 0;
  m_ticks_carry = 0;

  for (u32 i = 0; i < NUM_VOICES; i++)
  {
    Voice& v = m_voices[i];
    v.current_address = 0;
    std::fill_n(v.regs.index, NUM_VOICE_REGISTERS, u16(0));
    v.counter.bits = 0;
    v.current_block_flags.bits = 0;
    v.current_block_samples.fill(s16(0));
    v.previous_block_last_samples.fill(s16(0));
    v.adpcm_last_samples.fill(s32(0));
    v.adsr_phase = ADSRPhase::Off;
    v.adsr_target = {};
    v.adsr_ticks = 0;
    v.adsr_ticks_remaining = 0;
    v.adsr_step = 0;
    v.has_samples = false;
  }

  m_ram.fill(0);
}

bool SPU::DoState(StateWrapper& sw)
{
  sw.Do(&m_SPUCNT.bits);
  sw.Do(&m_SPUSTAT.bits);
  sw.Do(&m_transfer_address);
  sw.Do(&m_transfer_address_reg);
  sw.Do(&m_irq_address);
  sw.Do(&m_main_volume_left.bits);
  sw.Do(&m_main_volume_right.bits);
  sw.Do(&m_key_on_register);
  sw.Do(&m_key_off_register);
  sw.Do(&m_endx_register);
  sw.Do(&m_reverb_on_register);
  sw.Do(&m_ticks_carry);
  for (u32 i = 0; i < NUM_VOICES; i++)
  {
    Voice& v = m_voices[i];
    sw.Do(&v.current_address);
    sw.DoArray(v.regs.index, NUM_VOICE_REGISTERS);
    sw.Do(&v.counter.bits);
    sw.Do(&v.current_block_flags.bits);
    sw.Do(&v.current_block_samples);
    sw.Do(&v.previous_block_last_samples);
    sw.Do(&v.adpcm_last_samples);
    sw.Do(&v.adsr_phase);
    sw.DoPOD(&v.adsr_target);
    sw.Do(&v.adsr_ticks);
    sw.Do(&v.adsr_ticks_remaining);
    sw.Do(&v.adsr_step);
    sw.Do(&v.has_samples);
  }

  sw.DoBytes(m_ram.data(), RAM_SIZE);

  if (sw.IsReading())
    m_audio_stream->EmptyBuffers();

  return !sw.HasError();
}

u16 SPU::ReadRegister(u32 offset)
{
  if (offset < (0x1F801D80 - SPU_BASE))
    return ReadVoiceRegister(offset);

  switch (offset)
  {
    case 0x1F801D80 - SPU_BASE:
      return m_main_volume_left.bits;

    case 0x1F801D82 - SPU_BASE:
      return m_main_volume_right.bits;

    case 0x1F801D88 - SPU_BASE:
      return Truncate16(m_key_on_register);

    case 0x1F801D8A - SPU_BASE:
      return Truncate16(m_key_on_register >> 16);

    case 0x1F801D8C - SPU_BASE:
      return Truncate16(m_key_off_register);

    case 0x1F801D8E - SPU_BASE:
      return Truncate16(m_key_off_register >> 16);

    case 0x1F801D98 - SPU_BASE:
      return Truncate16(m_reverb_on_register);

    case 0x1F801D9A - SPU_BASE:
      return Truncate16(m_reverb_on_register >> 16);

    case 0x1F801DA6 - SPU_BASE:
      Log_DebugPrintf("SPU transfer address register -> 0x%04X", ZeroExtend32(m_transfer_address_reg));
      return m_transfer_address_reg;

    case 0x1F801DA8 - SPU_BASE:
      Log_ErrorPrintf("SPU transfer data register read");
      return UINT16_C(0xFFFF);

    case 0x1F801DAA - SPU_BASE:
      Log_DebugPrintf("SPU control register -> 0x%04X", ZeroExtend32(m_SPUCNT.bits));
      return m_SPUCNT.bits;

    case 0x1F801DAE - SPU_BASE:
      // Log_DebugPrintf("SPU status register -> 0x%04X", ZeroExtend32(m_SPUCNT.bits));
      return m_SPUSTAT.bits;

    default:
      Log_ErrorPrintf("Unknown SPU register read: offset 0x%X (address 0x%08X)", offset, offset | SPU_BASE);
      return UINT16_C(0xFFFF);
  }
}

void SPU::WriteRegister(u32 offset, u16 value)
{
  if (offset < (0x1F801D80 - SPU_BASE))
  {
    WriteVoiceRegister(offset, value);
    return;
  }

  switch (offset)
  {
    case 0x1F801DA6 - SPU_BASE:
    {
      Log_DebugPrintf("SPU transfer address register <- 0x%04X", ZeroExtend32(value));
      m_transfer_address_reg = value;
      m_transfer_address = (ZeroExtend32(value) << VOICE_ADDRESS_SHIFT) & RAM_MASK;
      return;
    }

    case 0x1F801DA8 - SPU_BASE:
    {
      Log_TracePrintf("SPU transfer data register <- 0x%04X (RAM offset 0x%08X)", ZeroExtend32(value),
                      m_transfer_address);
      RAMTransferWrite(value);
      return;
    }

    case 0x1F801DAA - SPU_BASE:
    {
      Log_DebugPrintf("SPU control register <- 0x%04X", ZeroExtend32(value));
      m_SPUCNT.bits = value;
      UpdateDMARequest();
      return;
    }

    case 0x1F801D80 - SPU_BASE:
    {
      Log_DebugPrintf("SPU main volume left <- 0x%04X", ZeroExtend32(value));
      m_system->Synchronize();
      m_main_volume_left.bits = value;
      return;
    }

    case 0x1F801D82 - SPU_BASE:
    {
      Log_DebugPrintf("SPU main volume right <- 0x%04X", ZeroExtend32(value));
      m_system->Synchronize();
      m_main_volume_right.bits = value;
      return;
    }

    case 0x1F801D88 - SPU_BASE:
    {
      Log_DebugPrintf("SPU key on low <- 0x%04X", ZeroExtend32(value));
      m_system->Synchronize();
      m_key_on_register = (m_key_on_register & 0xFFFF0000) | ZeroExtend32(value);

      u16 bits = value;
      for (u32 i = 0; i < 16; i++)
      {
        if (bits & 0x01)
        {
          Log_DebugPrintf("Voice %u key on", i);
          m_voices[i].KeyOn();
        }
        bits >>= 1;
      }
    }
    break;

    case 0x1F801D8A - SPU_BASE:
    {
      Log_DebugPrintf("SPU key on high <- 0x%04X", ZeroExtend32(value));
      m_system->Synchronize();
      m_key_on_register = (m_key_on_register & 0x0000FFFF) | (ZeroExtend32(value) << 16);

      u16 bits = value;
      for (u32 i = 16; i < NUM_VOICES; i++)
      {
        if (bits & 0x01)
        {
          Log_DebugPrintf("Voice %u key on", i);
          m_voices[i].KeyOn();
        }
        bits >>= 1;
      }
    }
    break;

    case 0x1F801D8C - SPU_BASE:
    {
      Log_DebugPrintf("SPU key off low <- 0x%04X", ZeroExtend32(value));
      m_system->Synchronize();
      m_key_on_register = (m_key_on_register & 0xFFFF0000) | ZeroExtend32(value);

      u16 bits = value;
      for (u32 i = 0; i < 16; i++)
      {
        if (bits & 0x01)
        {
          Log_DebugPrintf("Voice %u key off", i);
          m_voices[i].KeyOff();
        }
        bits >>= 1;
      }
    }
    break;

    case 0x1F801D8E - SPU_BASE:
    {
      Log_DebugPrintf("SPU key off high <- 0x%04X", ZeroExtend32(value));
      m_system->Synchronize();
      m_key_on_register = (m_key_on_register & 0x0000FFFF) | (ZeroExtend32(value) << 16);

      u16 bits = value;
      for (u32 i = 16; i < NUM_VOICES; i++)
      {
        if (bits & 0x01)
        {
          Log_DebugPrintf("Voice %u key off", i);
          m_voices[i].KeyOff();
        }
        bits >>= 1;
      }
    }
    break;

    case 0x1F801D98 - SPU_BASE:
    {
      Log_DebugPrintf("SPU reverb on register <- 0x%04X", ZeroExtend32(value));
      m_system->Synchronize();
      m_reverb_on_register = (m_reverb_on_register & 0xFFFF0000) | ZeroExtend32(value);
    }
    break;

    case 0x1F801D9A - SPU_BASE:
    {
      Log_DebugPrintf("SPU reverb off register <- 0x%04X", ZeroExtend32(value));
      m_system->Synchronize();
      m_reverb_on_register = (m_reverb_on_register & 0x0000FFFF) | (ZeroExtend32(value) << 16);
    }
    break;

      // read-only registers
    case 0x1F801DAE - SPU_BASE:
    {
      return;
    }

    default:
    {
      Log_ErrorPrintf("Unknown SPU register write: offset 0x%X (address 0x%08X) value 0x%04X", offset,
                      offset | SPU_BASE, ZeroExtend32(value));
      return;
    }
  }
}

u16 SPU::ReadVoiceRegister(u32 offset)
{
  const u32 reg_index = (offset % 0x10) / 2; //(offset & 0x0F) / 2;
  const u32 voice_index = (offset / 0x10);   //((offset >> 4) & 0x1F);
  Assert(voice_index < 24);

  return m_voices[voice_index].regs.index[reg_index];
}

void SPU::WriteVoiceRegister(u32 offset, u16 value)
{
  // per-voice registers
  const u32 reg_index = (offset % 0x10);
  const u32 voice_index = (offset / 0x10);
  Assert(voice_index < 24);

  Voice& voice = m_voices[voice_index];
  if (voice.IsOn())
    m_system->Synchronize();

  switch (reg_index)
  {
    case 0x00: // volume left
    {
      Log_DebugPrintf("SPU voice %u volume left <- 0x%04X", voice_index, value);
      voice.regs.volume_left.bits = value;
    }
    break;

    case 0x02: // volume right
    {
      Log_DebugPrintf("SPU voice %u volume right <- 0x%04X", voice_index, value);
      voice.regs.volume_right.bits = value;
    }
    break;

    case 0x04: // sample rate
    {
      Log_DebugPrintf("SPU voice %u ADPCM sample rate <- 0x%04X", voice_index, value);
      voice.regs.adpcm_sample_rate = value;
    }
    break;

    case 0x06: // start address
    {
      Log_DebugPrintf("SPU voice %u ADPCM start address <- 0x%04X", voice_index, value);
      voice.regs.adpcm_start_address = value;
    }
    break;

    case 0x08: // adsr low
    {
      Log_DebugPrintf("SPU voice %u ADSR low <- 0x%04X", voice_index, value);
      voice.regs.adsr.bits_low = value;
    }
    break;

    case 0x0A: // adsr high
    {
      Log_DebugPrintf("SPU voice %u ADSR high <- 0x%04X", voice_index, value);
      voice.regs.adsr.bits_high = value;
    }
    break;

    case 0x0C: // adsr volume
    {
      Log_DebugPrintf("SPU voice %u ADSR volume <- 0x%04X", voice_index, value);
      voice.regs.adsr_volume = value;
    }
    break;

    case 0x0E: // repeat address
    {
      Log_DebugPrintf("SPU voice %u ADPCM repeat address <- 0x%04X", voice_index, value);
      voice.regs.adpcm_repeat_address = value;
    }
    break;

    default:
    {
      Log_ErrorPrintf("Unknown SPU voice %u register write: offset 0x%X (address 0x%08X) value 0x%04X", offset,
                      voice_index, offset | SPU_BASE, ZeroExtend32(value));
    }
    break;
  }
}

void SPU::DMARead(u32* words, u32 word_count)
{
  // test for wrap-around
  if ((m_transfer_address & ~RAM_MASK) != ((m_transfer_address + (word_count * sizeof(u32))) & ~RAM_MASK))
  {
    // this could still be optimized to copy in two parts - end/start, but is unlikely.
    for (u32 i = 0; i < word_count; i++)
    {
      const u16 lsb = RAMTransferRead();
      const u16 msb = RAMTransferRead();
      words[i] = ZeroExtend32(lsb) | (ZeroExtend32(msb) << 16);
    }
  }
  else
  {
    std::memcpy(words, &m_ram[m_transfer_address], sizeof(u32) * word_count);
    m_transfer_address = (m_transfer_address + (sizeof(u32) * word_count)) & RAM_MASK;
  }
}

void SPU::DMAWrite(const u32* words, u32 word_count)
{
  // test for wrap-around
  if ((m_transfer_address & ~RAM_MASK) != ((m_transfer_address + (word_count * sizeof(u32))) & ~RAM_MASK))
  {
    // this could still be optimized to copy in two parts - end/start, but is unlikely.
    for (u32 i = 0; i < word_count; i++)
    {
      const u32 value = words[i];
      RAMTransferWrite(Truncate16(value));
      RAMTransferWrite(Truncate16(value >> 16));
    }
  }
  else
  {
    std::memcpy(&m_ram[m_transfer_address], words, sizeof(u32) * word_count);
    m_transfer_address = (m_transfer_address + (sizeof(u32) * word_count)) & RAM_MASK;
  }
}

void SPU::UpdateDMARequest()
{
  const RAMTransferMode mode = m_SPUCNT.ram_transfer_mode;
  const bool request = (mode == RAMTransferMode::DMAWrite || mode == RAMTransferMode::DMARead);
  m_dma->SetRequest(DMA::Channel::SPU, request);
}

u16 SPU::RAMTransferRead()
{
  u16 value;
  std::memcpy(&value, &m_ram[m_transfer_address], sizeof(value));
  m_transfer_address = (m_transfer_address + sizeof(value)) & RAM_MASK;
  return value;
}

void SPU::RAMTransferWrite(u16 value)
{
  Log_TracePrintf("SPU RAM @ 0x%08X (voice 0x%04X) <- 0x%04X", m_transfer_address,
                  m_transfer_address >> VOICE_ADDRESS_SHIFT, ZeroExtend32(value));
  std::memcpy(&m_ram[m_transfer_address], &value, sizeof(value));
  m_transfer_address = (m_transfer_address + sizeof(value)) & RAM_MASK;
}

void SPU::Execute(TickCount ticks)
{
  TickCount num_samples = (ticks + m_ticks_carry) / SYSCLK_TICKS_PER_SPU_TICK;
  m_ticks_carry = (ticks + m_ticks_carry) % SYSCLK_TICKS_PER_SPU_TICK;
  if (num_samples == 0 || (!m_SPUCNT.enable && !m_SPUCNT.cd_audio_enable))
    return;

  for (TickCount i = 0; i < num_samples; i++)
    GenerateSample();
}

void SPU::Voice::KeyOn()
{
  current_address = regs.adpcm_start_address;
  regs.adsr_volume = 0;
  has_samples = false;
  SetADSRPhase(ADSRPhase::Attack);
}

void SPU::Voice::KeyOff()
{
  if (adsr_phase == ADSRPhase::Off)
    return;

  SetADSRPhase(ADSRPhase::Release);
}

SPU::ADSRPhase SPU::GetNextADSRPhase(ADSRPhase phase)
{
  switch (phase)
  {
    case ADSRPhase::Attack:
      // attack -> decay
      return ADSRPhase::Decay;

    case ADSRPhase::Decay:
      // decay -> sustain
      return ADSRPhase::Sustain;

    case ADSRPhase::Sustain:
      // sustain stays in sustain until key off
      return ADSRPhase::Sustain;

    default:
    case ADSRPhase::Release:
      // end of release disables the voice
      return ADSRPhase::Off;
  }
}

void SPU::Voice::SetADSRPhase(ADSRPhase phase)
{
  adsr_phase = phase;
  switch (phase)
  {
    case ADSRPhase::Off:
      adsr_target = {};
      break;

    case ADSRPhase::Attack:
      adsr_target.level = 32767; // 0 -> max
      adsr_target.step = regs.adsr.attack_step + 4;
      adsr_target.shift = regs.adsr.attack_shift;
      adsr_target.decreasing = false;
      adsr_target.exponential = regs.adsr.attack_exponential;
      break;

    case ADSRPhase::Decay:
      adsr_target.level = (u32(regs.adsr.sustain_level.GetValue()) + 1) * 0x800; // max -> sustain level
      adsr_target.step = 0;
      adsr_target.shift = regs.adsr.decay_shift;
      adsr_target.decreasing = true;
      adsr_target.exponential = true;
      break;

    case ADSRPhase::Sustain:
      adsr_target.level = regs.adsr.sustain_direction_decrease ? -1 : 1;
      adsr_target.step = 0;
      adsr_target.shift = regs.adsr.sustain_shift;
      adsr_target.decreasing = regs.adsr.sustain_direction_decrease;
      adsr_target.exponential = regs.adsr.sustain_exponential;
      break;

    case ADSRPhase::Release:
      adsr_target.level = 0;
      adsr_target.step = 0;
      adsr_target.shift = regs.adsr.release_shift;
      adsr_target.decreasing = true;
      adsr_target.exponential = regs.adsr.release_exponential;
      break;

    default:
      break;
  }

  const s16 step = adsr_target.decreasing ? (-8 + adsr_target.step) : (7 - adsr_target.step);
  adsr_ticks = 1 << std::max<s16>(0, adsr_target.shift - 11);
  adsr_ticks_remaining = adsr_ticks;
  adsr_step = step << std::max<s16>(0, 11 - adsr_target.shift);
}

void SPU::Voice::TickADSR()
{
  adsr_ticks_remaining--;
  if (adsr_ticks_remaining <= 0)
  {
    const s32 new_volume = s32(regs.adsr_volume) + s32(adsr_step);
    regs.adsr_volume = static_cast<s16>(std::clamp<s32>(new_volume, ADSR_MIN_VOLUME, ADSR_MAX_VOLUME));

    const bool reached_target =
      adsr_target.decreasing ? (new_volume <= adsr_target.level) : (new_volume >= adsr_target.level);
    if (adsr_phase != ADSRPhase::Sustain && reached_target)
    {
      // next phase
      SetADSRPhase(GetNextADSRPhase(adsr_phase));
    }
    else
    {
      adsr_ticks_remaining = adsr_ticks;
    }
  }
}

void SPU::Voice::DecodeBlock(const ADPCMBlock& block)
{
  static constexpr std::array<s32, 5> filter_table_pos = {{0, 60, 115, 98, 122}};
  static constexpr std::array<s32, 5> filter_table_neg = {{0, 0, -52, -55, -60}};

  // store samples needed for interpolation
  previous_block_last_samples[2] = current_block_samples[NUM_SAMPLES_PER_ADPCM_BLOCK - 1];
  previous_block_last_samples[1] = current_block_samples[NUM_SAMPLES_PER_ADPCM_BLOCK - 2];
  previous_block_last_samples[0] = current_block_samples[NUM_SAMPLES_PER_ADPCM_BLOCK - 3];

  // pre-lookup
  const u8 shift = block.GetShift();
  const u8 filter_index = block.GetFilter();
  const s32 filter_pos = filter_table_pos[filter_index];
  const s32 filter_neg = filter_table_neg[filter_index];
  s32 last_samples[2] = {adpcm_last_samples[0], adpcm_last_samples[1]};

  // samples
  for (u32 i = 0; i < NUM_SAMPLES_PER_ADPCM_BLOCK; i++)
  {
    // extend 4-bit to 16-bit, apply shift from header and mix in previous samples
    const s16 sample = static_cast<s16>(ZeroExtend16(block.GetNibble(i)) << 12) >> shift;
    const s32 interp_sample = s32(sample) + ((last_samples[0] * filter_pos) + (last_samples[1] * filter_neg) + 32) / 64;

    current_block_samples[i] = Clamp16(interp_sample);
    last_samples[1] = last_samples[0];
    last_samples[0] = interp_sample;
  }

  std::copy(last_samples, last_samples + countof(last_samples), adpcm_last_samples.begin());
  current_block_flags.bits = block.flags.bits;
}

SPU::SampleFormat SPU::Voice::SampleBlock(s32 index) const
{
  if (index < 0)
  {
    DebugAssert(index >= -3);
    return previous_block_last_samples[index + 3];
  }

  return current_block_samples[index];
}

s16 SPU::Voice::Interpolate() const
{
  static constexpr std::array<s32, 0x200> gauss = {{
    -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, //
    -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, //
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001, //
    0x0001, 0x0001, 0x0001, 0x0002, 0x0002, 0x0002, 0x0003, 0x0003, //
    0x0003, 0x0004, 0x0004, 0x0005, 0x0005, 0x0006, 0x0007, 0x0007, //
    0x0008, 0x0009, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, //
    0x000F, 0x0010, 0x0011, 0x0012, 0x0013, 0x0015, 0x0016, 0x0018, // entry
    0x0019, 0x001B, 0x001C, 0x001E, 0x0020, 0x0021, 0x0023, 0x0025, // 000..07F
    0x0027, 0x0029, 0x002C, 0x002E, 0x0030, 0x0033, 0x0035, 0x0038, //
    0x003A, 0x003D, 0x0040, 0x0043, 0x0046, 0x0049, 0x004D, 0x0050, //
    0x0054, 0x0057, 0x005B, 0x005F, 0x0063, 0x0067, 0x006B, 0x006F, //
    0x0074, 0x0078, 0x007D, 0x0082, 0x0087, 0x008C, 0x0091, 0x0096, //
    0x009C, 0x00A1, 0x00A7, 0x00AD, 0x00B3, 0x00BA, 0x00C0, 0x00C7, //
    0x00CD, 0x00D4, 0x00DB, 0x00E3, 0x00EA, 0x00F2, 0x00FA, 0x0101, //
    0x010A, 0x0112, 0x011B, 0x0123, 0x012C, 0x0135, 0x013F, 0x0148, //
    0x0152, 0x015C, 0x0166, 0x0171, 0x017B, 0x0186, 0x0191, 0x019C, //
    0x01A8, 0x01B4, 0x01C0, 0x01CC, 0x01D9, 0x01E5, 0x01F2, 0x0200, //
    0x020D, 0x021B, 0x0229, 0x0237, 0x0246, 0x0255, 0x0264, 0x0273, //
    0x0283, 0x0293, 0x02A3, 0x02B4, 0x02C4, 0x02D6, 0x02E7, 0x02F9, //
    0x030B, 0x031D, 0x0330, 0x0343, 0x0356, 0x036A, 0x037E, 0x0392, //
    0x03A7, 0x03BC, 0x03D1, 0x03E7, 0x03FC, 0x0413, 0x042A, 0x0441, //
    0x0458, 0x0470, 0x0488, 0x04A0, 0x04B9, 0x04D2, 0x04EC, 0x0506, //
    0x0520, 0x053B, 0x0556, 0x0572, 0x058E, 0x05AA, 0x05C7, 0x05E4, // entry
    0x0601, 0x061F, 0x063E, 0x065C, 0x067C, 0x069B, 0x06BB, 0x06DC, // 080..0FF
    0x06FD, 0x071E, 0x0740, 0x0762, 0x0784, 0x07A7, 0x07CB, 0x07EF, //
    0x0813, 0x0838, 0x085D, 0x0883, 0x08A9, 0x08D0, 0x08F7, 0x091E, //
    0x0946, 0x096F, 0x0998, 0x09C1, 0x09EB, 0x0A16, 0x0A40, 0x0A6C, //
    0x0A98, 0x0AC4, 0x0AF1, 0x0B1E, 0x0B4C, 0x0B7A, 0x0BA9, 0x0BD8, //
    0x0C07, 0x0C38, 0x0C68, 0x0C99, 0x0CCB, 0x0CFD, 0x0D30, 0x0D63, //
    0x0D97, 0x0DCB, 0x0E00, 0x0E35, 0x0E6B, 0x0EA1, 0x0ED7, 0x0F0F, //
    0x0F46, 0x0F7F, 0x0FB7, 0x0FF1, 0x102A, 0x1065, 0x109F, 0x10DB, //
    0x1116, 0x1153, 0x118F, 0x11CD, 0x120B, 0x1249, 0x1288, 0x12C7, //
    0x1307, 0x1347, 0x1388, 0x13C9, 0x140B, 0x144D, 0x1490, 0x14D4, //
    0x1517, 0x155C, 0x15A0, 0x15E6, 0x162C, 0x1672, 0x16B9, 0x1700, //
    0x1747, 0x1790, 0x17D8, 0x1821, 0x186B, 0x18B5, 0x1900, 0x194B, //
    0x1996, 0x19E2, 0x1A2E, 0x1A7B, 0x1AC8, 0x1B16, 0x1B64, 0x1BB3, //
    0x1C02, 0x1C51, 0x1CA1, 0x1CF1, 0x1D42, 0x1D93, 0x1DE5, 0x1E37, //
    0x1E89, 0x1EDC, 0x1F2F, 0x1F82, 0x1FD6, 0x202A, 0x207F, 0x20D4, //
    0x2129, 0x217F, 0x21D5, 0x222C, 0x2282, 0x22DA, 0x2331, 0x2389, // entry
    0x23E1, 0x2439, 0x2492, 0x24EB, 0x2545, 0x259E, 0x25F8, 0x2653, // 100..17F
    0x26AD, 0x2708, 0x2763, 0x27BE, 0x281A, 0x2876, 0x28D2, 0x292E, //
    0x298B, 0x29E7, 0x2A44, 0x2AA1, 0x2AFF, 0x2B5C, 0x2BBA, 0x2C18, //
    0x2C76, 0x2CD4, 0x2D33, 0x2D91, 0x2DF0, 0x2E4F, 0x2EAE, 0x2F0D, //
    0x2F6C, 0x2FCC, 0x302B, 0x308B, 0x30EA, 0x314A, 0x31AA, 0x3209, //
    0x3269, 0x32C9, 0x3329, 0x3389, 0x33E9, 0x3449, 0x34A9, 0x3509, //
    0x3569, 0x35C9, 0x3629, 0x3689, 0x36E8, 0x3748, 0x37A8, 0x3807, //
    0x3867, 0x38C6, 0x3926, 0x3985, 0x39E4, 0x3A43, 0x3AA2, 0x3B00, //
    0x3B5F, 0x3BBD, 0x3C1B, 0x3C79, 0x3CD7, 0x3D35, 0x3D92, 0x3DEF, //
    0x3E4C, 0x3EA9, 0x3F05, 0x3F62, 0x3FBD, 0x4019, 0x4074, 0x40D0, //
    0x412A, 0x4185, 0x41DF, 0x4239, 0x4292, 0x42EB, 0x4344, 0x439C, //
    0x43F4, 0x444C, 0x44A3, 0x44FA, 0x4550, 0x45A6, 0x45FC, 0x4651, //
    0x46A6, 0x46FA, 0x474E, 0x47A1, 0x47F4, 0x4846, 0x4898, 0x48E9, //
    0x493A, 0x498A, 0x49D9, 0x4A29, 0x4A77, 0x4AC5, 0x4B13, 0x4B5F, //
    0x4BAC, 0x4BF7, 0x4C42, 0x4C8D, 0x4CD7, 0x4D20, 0x4D68, 0x4DB0, //
    0x4DF7, 0x4E3E, 0x4E84, 0x4EC9, 0x4F0E, 0x4F52, 0x4F95, 0x4FD7, // entry
    0x5019, 0x505A, 0x509A, 0x50DA, 0x5118, 0x5156, 0x5194, 0x51D0, // 180..1FF
    0x520C, 0x5247, 0x5281, 0x52BA, 0x52F3, 0x532A, 0x5361, 0x5397, //
    0x53CC, 0x5401, 0x5434, 0x5467, 0x5499, 0x54CA, 0x54FA, 0x5529, //
    0x5558, 0x5585, 0x55B2, 0x55DE, 0x5609, 0x5632, 0x565B, 0x5684, //
    0x56AB, 0x56D1, 0x56F6, 0x571B, 0x573E, 0x5761, 0x5782, 0x57A3, //
    0x57C3, 0x57E2, 0x57FF, 0x581C, 0x5838, 0x5853, 0x586D, 0x5886, //
    0x589E, 0x58B5, 0x58CB, 0x58E0, 0x58F4, 0x5907, 0x5919, 0x592A, //
    0x593A, 0x5949, 0x5958, 0x5965, 0x5971, 0x597C, 0x5986, 0x598F, //
    0x5997, 0x599E, 0x59A4, 0x59A9, 0x59AD, 0x59B0, 0x59B2, 0x59B3  //
  }};

  const u8 i = counter.interpolation_index;
  const s32 s = static_cast<s32>(ZeroExtend32(counter.sample_index.GetValue()));

  s16 out = s16(gauss[0x0FF - i] * s32(SampleBlock(s - 3)) >> 15);
  out += s16(gauss[0x1FF - i] * s32(SampleBlock(s - 2)) >> 15);
  out += s16(gauss[0x100 + i] * s32(SampleBlock(s - 1)) >> 15);
  out += s16(gauss[0x000 + i] * s32(SampleBlock(s - 0)) >> 15);
  return out;
}

void SPU::ReadADPCMBlock(u16 address, ADPCMBlock* block)
{
  u32 ram_address = (ZeroExtend32(address) * 8) & RAM_MASK;

  // 16 bytes, so 2 8-byte blocks for the interrupt check
  if (m_SPUCNT.irq9_enable)
  {
    if (m_irq_address == address || m_irq_address == (address + 1))
    {
      Log_DebugPrintf("SPU IRQ at address 0x%08X", ram_address);
      m_SPUSTAT.irq9_flag = true;
      m_interrupt_controller->InterruptRequest(InterruptController::IRQ::SPU);
    }
  }

  // fast path - no wrap-around
  if ((ram_address + sizeof(ADPCMBlock)) <= RAM_SIZE)
  {
    std::memcpy(block, &m_ram[ram_address], sizeof(ADPCMBlock));
    return;
  }

  block->shift_filter.bits = m_ram[ram_address];
  ram_address = (ram_address + 1) & RAM_MASK;
  block->flags.bits = m_ram[ram_address];
  ram_address = (ram_address + 1) & RAM_MASK;
  for (u32 i = 0; i < 14; i++)
  {
    block->data[i] = m_ram[ram_address];
    ram_address = (ram_address + 1) & RAM_MASK;
  }
}

std::tuple<s32, s32> SPU::SampleVoice(u32 voice_index)
{
  Voice& voice = m_voices[voice_index];
  if (!voice.IsOn())
    return {};

  if (!voice.has_samples)
  {
    ADPCMBlock block;
    ReadADPCMBlock(voice.current_address, &block);
    voice.DecodeBlock(block);
    voice.has_samples = true;

    if (voice.current_block_flags.loop_start)
    {
      Log_TracePrintf("Voice %u loop start @ 0x%08X", voice_index, ZeroExtend32(voice.current_address));
      voice.regs.adpcm_repeat_address = voice.current_address;
    }
  }

  // TODO: Pulse modulation
  u16 step = voice.regs.adpcm_sample_rate;
  step = std::min<u16>(step, 0x4000);
  voice.counter.bits += step;

  if (voice.counter.sample_index >= NUM_SAMPLES_PER_ADPCM_BLOCK)
  {
    // next block
    voice.counter.sample_index -= NUM_SAMPLES_PER_ADPCM_BLOCK;
    voice.has_samples = false;

    // handle flags
    if (voice.current_block_flags.loop_end)
    {
      if (!voice.current_block_flags.loop_repeat)
      {
        Log_TracePrintf("Voice %u loop end+mute @ 0x%08X", voice_index, ZeroExtend32(voice.current_address));
        m_endx_register |= (u32(1) << voice_index);
        voice.KeyOff();
      }
      else
      {
        Log_TracePrintf("Voice %u loop end+repeat @ 0x%08X", voice_index, ZeroExtend32(voice.current_address));
        voice.current_address = voice.regs.adpcm_repeat_address;
      }
    }
    else
    {
      voice.current_address += 2;
    }
  }

  // interpolate/sample and apply ADSR volume
  const s32 sample = ApplyVolumeUnsaturated(voice.Interpolate(), voice.regs.adsr_volume);
  voice.TickADSR();

  // apply per-channel volume
  const s16 left = ApplyVolumeUnsaturated(sample, voice.regs.volume_left.GetVolume());
  const s16 right = ApplyVolumeUnsaturated(sample, voice.regs.volume_right.GetVolume());
  return std::make_tuple(left, right);
}

void SPU::EnsureCDAudioSpace(u32 num_samples)
{
  if (m_cd_audio_buffer.GetSpace() < (num_samples * 2))
  {
    Log_WarningPrintf("SPU CD Audio buffer overflow - writing %u samples with %u samples space", num_samples,
                      m_cd_audio_buffer.GetSpace() / 2);
    m_cd_audio_buffer.Remove((num_samples * 2) - m_cd_audio_buffer.GetSpace());
  }
}

void SPU::GenerateSample()
{
  s32 left_sum = 0;
  s32 right_sum = 0;
  if (m_SPUCNT.enable)
  {
    for (u32 i = 0; i < NUM_VOICES; i++)
    {
      const auto [left, right] = SampleVoice(i);
      left_sum += left;
      right_sum += right;
    }
  }

  // Mix in CD audio.
  // TODO: Volume control.
  if (m_SPUCNT.cd_audio_enable && !m_cd_audio_buffer.IsEmpty())
  {
    left_sum += s32(m_cd_audio_buffer.Pop());
    right_sum += s32(m_cd_audio_buffer.Pop());
  }

  // Log_DebugPrintf("SPU sample %d %d", left_sum, right_sum);
  AudioStream::SampleType samples[2] = {Clamp16(left_sum), Clamp16(right_sum)};
  m_audio_stream->WriteSamples(samples, 1);

#if 0
  static FILE* fp = nullptr;
  if (!fp)
    fp = std::fopen("D:\\spu.raw", "wb");
  if (fp)
  {
    std::fwrite(samples, sizeof(AudioStream::SampleType), 2, fp);
    std::fflush(fp);
  }
#endif
}

void SPU::DrawDebugWindow()
{
  if (!m_show_spu_state)
    return;

  ImGui::SetNextWindowSize(ImVec2(800, 400), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("SPU State", &m_show_spu_state))
  {
    ImGui::End();
    return;
  }

  // draw voice states
  if (ImGui::CollapsingHeader("Voice State", ImGuiTreeNodeFlags_DefaultOpen))
  {
    static constexpr u32 NUM_COLUMNS = 12;

    ImGui::Columns(NUM_COLUMNS);

    // headers
    static constexpr std::array<const char*, NUM_COLUMNS> column_titles = {
      {"#", "InterpIndex", "SampleIndex", "CurAddr", "StartAddr", "RepeatAddr", "SampleRate", "VolLeft", "VolRight",
       "ADSR", "ADSRPhase", "ADSRVol"}};
    static constexpr std::array<const char*, 5> adsr_phases = {{"Off", "Attack", "Decay", "Sustain", "Release"}};
    for (u32 i = 0; i < NUM_COLUMNS; i++)
    {
      ImGui::TextUnformatted(column_titles[i]);
      ImGui::NextColumn();
    }

    // states
    for (u32 voice_index = 0; voice_index < NUM_VOICES; voice_index++)
    {
      const Voice& v = m_voices[voice_index];
      ImVec4 color = v.IsOn() ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
      ImGui::TextColored(color, "%u", ZeroExtend32(voice_index));
      ImGui::NextColumn();
      ImGui::TextColored(color, "%u", ZeroExtend32(v.counter.interpolation_index.GetValue()));
      ImGui::NextColumn();
      ImGui::TextColored(color, "%u", ZeroExtend32(v.counter.sample_index.GetValue()));
      ImGui::NextColumn();
      ImGui::TextColored(color, "%04X", ZeroExtend32(v.current_address));
      ImGui::NextColumn();
      ImGui::TextColored(color, "%04X", ZeroExtend32(v.regs.adpcm_start_address));
      ImGui::NextColumn();
      ImGui::TextColored(color, "%04X", ZeroExtend32(v.regs.adpcm_repeat_address));
      ImGui::NextColumn();
      ImGui::TextColored(color, "%.2f", (float(v.regs.adpcm_sample_rate) / 16383.0f) * 44100.0f);
      ImGui::NextColumn();
      ImGui::TextColored(color, "%04X", ZeroExtend32(v.regs.volume_left.bits));
      ImGui::NextColumn();
      ImGui::TextColored(color, "%04X", ZeroExtend32(v.regs.volume_right.bits));
      ImGui::NextColumn();
      ImGui::TextColored(color, "%08X", v.regs.adsr.bits);
      ImGui::NextColumn();
      ImGui::TextColored(color, adsr_phases[static_cast<u8>(v.adsr_phase)]);
      ImGui::NextColumn();
      ImGui::TextColored(color, "%d", ZeroExtend32(v.regs.adsr_volume));
      ImGui::NextColumn();
    }

    ImGui::Columns(1);
  }

  ImGui::End();
}

void SPU::DrawDebugMenu()
{
  // TODO: Show RAM, etc.
  ImGui::MenuItem("SPU", nullptr, &m_show_spu_state);
}
