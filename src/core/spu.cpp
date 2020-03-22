#include "spu.h"
#include "common/audio_stream.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "common/wav_writer.h"
#include "dma.h"
#include "host_interface.h"
#include "interrupt_controller.h"
#include "system.h"
#include <imgui.h>
Log_SetChannel(SPU);

// TODO:
//   - Noise

SPU::SPU() = default;

SPU::~SPU() = default;

void SPU::Initialize(System* system, DMA* dma, InterruptController* interrupt_controller)
{
  m_system = system;
  m_dma = dma;
  m_interrupt_controller = interrupt_controller;
  m_tick_event = m_system->CreateTimingEvent("SPU Sample", SYSCLK_TICKS_PER_SPU_TICK, SYSCLK_TICKS_PER_SPU_TICK,
                                             std::bind(&SPU::Execute, this, std::placeholders::_1), false);
}

void SPU::Reset()
{
  m_tick_counter = 0;
  m_ticks_carry = 0;

  m_SPUCNT.bits = 0;
  m_SPUSTAT.bits = 0;
  m_transfer_address = 0;
  m_transfer_address_reg = 0;
  m_irq_address = 0;
  m_capture_buffer_position = 0;
  m_main_volume_left_reg.bits = 0;
  m_main_volume_right_reg.bits = 0;
  m_main_volume_left = {};
  m_main_volume_right = {};
  m_cd_audio_volume_left = 0;
  m_cd_audio_volume_right = 0;
  m_key_on_register = 0;
  m_key_off_register = 0;
  m_endx_register = 0;
  m_noise_mode_register = 0;
  m_pitch_modulation_enable_register = 0;

  m_reverb_on_register = 0;
  m_reverb_registers = {};
  m_reverb_registers.mBASE = 0xE128;
  m_reverb_current_address = ZeroExtend32(m_reverb_registers.mBASE) * 8;

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
    v.SetADSRPhase(ADSRPhase::Off);
    v.has_samples = false;
  }

  m_voice_key_on_off_delay.fill(0);

  m_ram.fill(0);
  UpdateEventInterval();
}

bool SPU::DoState(StateWrapper& sw)
{
  sw.Do(&m_tick_counter);
  sw.Do(&m_ticks_carry);
  sw.Do(&m_SPUCNT.bits);
  sw.Do(&m_SPUSTAT.bits);
  sw.Do(&m_transfer_control.bits);
  sw.Do(&m_transfer_address);
  sw.Do(&m_transfer_address_reg);
  sw.Do(&m_irq_address);
  sw.Do(&m_capture_buffer_position);
  sw.Do(&m_main_volume_left_reg.bits);
  sw.Do(&m_main_volume_right_reg.bits);
  sw.DoPOD(&m_main_volume_left);
  sw.DoPOD(&m_main_volume_right);
  sw.Do(&m_cd_audio_volume_left);
  sw.Do(&m_cd_audio_volume_right);
  sw.Do(&m_key_on_register);
  sw.Do(&m_key_off_register);
  sw.Do(&m_endx_register);
  sw.Do(&m_noise_mode_register);
  sw.Do(&m_reverb_on_register);
  sw.Do(&m_reverb_current_address);
  sw.DoArray(m_reverb_registers.rev, NUM_REVERB_REGS);
  sw.Do(&m_reverb_left_input);
  sw.Do(&m_reverb_right_input);
  sw.Do(&m_reverb_left_output);
  sw.Do(&m_reverb_right_output);
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
    sw.Do(&v.last_amplitude);
    sw.DoPOD(&v.left_volume);
    sw.DoPOD(&v.right_volume);
    sw.DoPOD(&v.adsr_envelope);
    sw.Do(&v.adsr_phase);
    sw.Do(&v.adsr_target);
    sw.Do(&v.has_samples);
  }

  sw.Do(&m_voice_key_on_off_delay);

  sw.DoBytes(m_ram.data(), RAM_SIZE);

  if (sw.IsReading())
  {
    m_system->GetHostInterface()->GetAudioStream()->EmptyBuffers();
    UpdateEventInterval();
  }

  return !sw.HasError();
}

u16 SPU::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0x1F801D80 - SPU_BASE:
      return m_main_volume_left_reg.bits;

    case 0x1F801D82 - SPU_BASE:
      return m_main_volume_right_reg.bits;

    case 0x1F801D84 - SPU_BASE:
      return m_reverb_registers.vLOUT;

    case 0x1F801D86 - SPU_BASE:
      return m_reverb_registers.vROUT;

    case 0x1F801D88 - SPU_BASE:
      return Truncate16(m_key_on_register);

    case 0x1F801D8A - SPU_BASE:
      return Truncate16(m_key_on_register >> 16);

    case 0x1F801D8C - SPU_BASE:
      return Truncate16(m_key_off_register);

    case 0x1F801D8E - SPU_BASE:
      return Truncate16(m_key_off_register >> 16);

    case 0x1F801D90 - SPU_BASE:
      return Truncate16(m_pitch_modulation_enable_register);

    case 0x1F801D92 - SPU_BASE:
      return Truncate16(m_pitch_modulation_enable_register >> 16);

    case 0x1F801D94 - SPU_BASE:
      return Truncate16(m_noise_mode_register);

    case 0x1F801D96 - SPU_BASE:
      return Truncate16(m_noise_mode_register >> 16);

    case 0x1F801D98 - SPU_BASE:
      return Truncate16(m_reverb_on_register);

    case 0x1F801D9A - SPU_BASE:
      return Truncate16(m_reverb_on_register >> 16);

    case 0x1F801DA2 - SPU_BASE:
      return m_reverb_registers.mBASE;

    case 0x1F801DA4 - SPU_BASE:
      Log_DebugPrintf("SPU IRQ address -> 0x%04X", ZeroExtend32(m_irq_address));
      return m_irq_address;

    case 0x1F801DA6 - SPU_BASE:
      Log_DebugPrintf("SPU transfer address register -> 0x%04X", ZeroExtend32(m_transfer_address_reg));
      return m_transfer_address_reg;

    case 0x1F801DA8 - SPU_BASE:
      Log_ErrorPrintf("SPU transfer data register read");
      return UINT16_C(0xFFFF);

    case 0x1F801DAA - SPU_BASE:
      Log_DebugPrintf("SPU control register -> 0x%04X", ZeroExtend32(m_SPUCNT.bits));
      return m_SPUCNT.bits;

    case 0x1F801DAC - SPU_BASE:
      Log_DebugPrintf("SPU transfer control register -> 0x%04X", ZeroExtend32(m_transfer_control.bits));
      return m_transfer_control.bits;

    case 0x1F801DAE - SPU_BASE:
      // Log_DebugPrintf("SPU status register -> 0x%04X", ZeroExtend32(m_SPUCNT.bits));
      m_tick_event->InvokeEarly();
      return m_SPUSTAT.bits;

    case 0x1F801DB0 - SPU_BASE:
      return m_cd_audio_volume_left;

    case 0x1F801DB2 - SPU_BASE:
      return m_cd_audio_volume_right;

    default:
    {
      if (offset < (0x1F801D80 - SPU_BASE))
        return ReadVoiceRegister(offset);

      if (offset >= (0x1F801DC0 - SPU_BASE) && offset <= (0x1F801DFE - SPU_BASE))
        return m_reverb_registers.rev[(offset - (0x1F801DC0 - SPU_BASE)) / 2];

      Log_ErrorPrintf("Unknown SPU register read: offset 0x%X (address 0x%08X)", offset, offset | SPU_BASE);
      return UINT16_C(0xFFFF);
    }
  }
}

void SPU::WriteRegister(u32 offset, u16 value)
{
  switch (offset)
  {
    case 0x1F801D80 - SPU_BASE:
    {
      Log_DebugPrintf("SPU main volume left <- 0x%04X", ZeroExtend32(value));
      m_tick_event->InvokeEarly();
      m_main_volume_left_reg.bits = value;
      m_main_volume_left.Reset(m_main_volume_left_reg);
      return;
    }

    case 0x1F801D82 - SPU_BASE:
    {
      Log_DebugPrintf("SPU main volume right <- 0x%04X", ZeroExtend32(value));
      m_tick_event->InvokeEarly();
      m_main_volume_right_reg.bits = value;
      m_main_volume_right.Reset(m_main_volume_right_reg);
      return;
    }

    case 0x1F801D84 - SPU_BASE:
    {
      Log_DebugPrintf("SPU reverb output volume left <- 0x%04X", ZeroExtend32(value));
      m_tick_event->InvokeEarly();
      m_reverb_registers.vLOUT = value;
      return;
    }

    case 0x1F801D86 - SPU_BASE:
    {
      Log_DebugPrintf("SPU reverb output volume right <- 0x%04X", ZeroExtend32(value));
      m_tick_event->InvokeEarly();
      m_reverb_registers.vROUT = value;
      return;
    }

    case 0x1F801D88 - SPU_BASE:
    {
      Log_DebugPrintf("SPU key on low <- 0x%04X", ZeroExtend32(value));
      m_tick_event->InvokeEarly();
      m_key_on_register = (m_key_on_register & 0xFFFF0000) | ZeroExtend32(value);

      u16 bits = value;
      for (u32 i = 0; i < 16; i++)
      {
        if (bits & 0x01)
          VoiceKeyOn(i);

        bits >>= 1;
      }
    }
    break;

    case 0x1F801D8A - SPU_BASE:
    {
      Log_DebugPrintf("SPU key on high <- 0x%04X", ZeroExtend32(value));
      m_tick_event->InvokeEarly();
      m_key_on_register = (m_key_on_register & 0x0000FFFF) | (ZeroExtend32(value) << 16);

      u16 bits = value;
      for (u32 i = 16; i < NUM_VOICES; i++)
      {
        if (bits & 0x01)
          VoiceKeyOn(i);

        bits >>= 1;
      }
    }
    break;

    case 0x1F801D8C - SPU_BASE:
    {
      Log_DebugPrintf("SPU key off low <- 0x%04X", ZeroExtend32(value));
      m_tick_event->InvokeEarly();
      m_key_on_register = (m_key_on_register & 0xFFFF0000) | ZeroExtend32(value);

      u16 bits = value;
      for (u32 i = 0; i < 16; i++)
      {
        if (bits & 0x01)
          VoiceKeyOff(i);

        bits >>= 1;
      }
    }
    break;

    case 0x1F801D8E - SPU_BASE:
    {
      Log_DebugPrintf("SPU key off high <- 0x%04X", ZeroExtend32(value));
      m_tick_event->InvokeEarly();
      m_key_on_register = (m_key_on_register & 0x0000FFFF) | (ZeroExtend32(value) << 16);

      u16 bits = value;
      for (u32 i = 16; i < NUM_VOICES; i++)
      {
        if (bits & 0x01)
          VoiceKeyOff(i);

        bits >>= 1;
      }
    }
    break;

    case 0x1F801D90 - SPU_BASE:
    {
      m_tick_event->InvokeEarly();
      m_pitch_modulation_enable_register = (m_pitch_modulation_enable_register & 0xFFFF0000) | ZeroExtend32(value);
      Log_DebugPrintf("SPU pitch modulation enable register <- 0x%08X", m_pitch_modulation_enable_register);
    }
    break;

    case 0x1F801D92 - SPU_BASE:
    {
      m_tick_event->InvokeEarly();
      m_pitch_modulation_enable_register =
        (m_pitch_modulation_enable_register & 0x0000FFFF) | (ZeroExtend32(value) << 16);
      Log_DebugPrintf("SPU pitch modulation enable register <- 0x%08X", m_pitch_modulation_enable_register);
    }
    break;

    case 0x1F801D94 - SPU_BASE:
    {
      Log_DebugPrintf("SPU noise mode register <- 0x%04X", ZeroExtend32(value));
      m_tick_event->InvokeEarly();
      m_noise_mode_register = (m_noise_mode_register & 0xFFFF0000) | ZeroExtend32(value);
    }
    break;

    case 0x1F801D96 - SPU_BASE:
    {
      Log_DebugPrintf("SPU noise mode register <- 0x%04X", ZeroExtend32(value));
      m_tick_event->InvokeEarly();
      m_noise_mode_register = (m_noise_mode_register & 0x0000FFFF) | (ZeroExtend32(value) << 16);
    }
    break;

    case 0x1F801D98 - SPU_BASE:
    {
      Log_DebugPrintf("SPU reverb on register <- 0x%04X", ZeroExtend32(value));
      m_tick_event->InvokeEarly();
      m_reverb_on_register = (m_reverb_on_register & 0xFFFF0000) | ZeroExtend32(value);
    }
    break;

    case 0x1F801D9A - SPU_BASE:
    {
      Log_DebugPrintf("SPU reverb off register <- 0x%04X", ZeroExtend32(value));
      m_tick_event->InvokeEarly();
      m_reverb_on_register = (m_reverb_on_register & 0x0000FFFF) | (ZeroExtend32(value) << 16);
    }
    break;

    case 0x1F801DA2 - SPU_BASE:
    {
      Log_DebugPrintf("SPU reverb base address < 0x%04X", ZeroExtend32(value));
      m_tick_event->InvokeEarly();
      m_reverb_registers.mBASE = value;
      m_reverb_current_address = ZeroExtend32(m_reverb_registers.mBASE) * 8u;
    }
    break;

    case 0x1F801DA4 - SPU_BASE:
    {
      Log_DebugPrintf("SPU IRQ address register <- 0x%04X", ZeroExtend32(value));
      m_tick_event->InvokeEarly();
      m_irq_address = value;
      return;
    }

    case 0x1F801DA6 - SPU_BASE:
    {
      Log_DebugPrintf("SPU transfer address register <- 0x%04X", ZeroExtend32(value));
      m_transfer_address_reg = value;
      m_transfer_address = ZeroExtend32(value) * 8;
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
      m_tick_event->InvokeEarly(true);

      m_SPUCNT.bits = value;
      m_SPUSTAT.mode = m_SPUCNT.mode.GetValue();
      m_SPUSTAT.dma_read_write_request = m_SPUCNT.ram_transfer_mode >= RAMTransferMode::DMAWrite;

      if (!m_SPUCNT.irq9_enable)
        m_SPUSTAT.irq9_flag = false;

      UpdateDMARequest();
      UpdateEventInterval();
      return;
    }

    case 0x1F801DAC - SPU_BASE:
    {
      Log_DebugPrintf("SPU transfer control register <- 0x%04X", ZeroExtend32(value));
      m_transfer_control.bits = value;
      return;
    }

    case 0x1F801DB0 - SPU_BASE:
    {
      Log_DebugPrintf("SPU left cd audio register <- 0x%04X", ZeroExtend32(value));
      m_tick_event->InvokeEarly();
      m_cd_audio_volume_left = value;
    }
    break;

    case 0x1F801DB2 - SPU_BASE:
    {
      Log_DebugPrintf("SPU right cd audio register <- 0x%04X", ZeroExtend32(value));
      m_tick_event->InvokeEarly();
      m_cd_audio_volume_right = value;
    }
    break;

      // read-only registers
    case 0x1F801DAE - SPU_BASE:
    {
      return;
    }

    default:
    {
      if (offset < (0x1F801D80 - SPU_BASE))
      {
        WriteVoiceRegister(offset, value);
        return;
      }

      if (offset >= (0x1F801DC0 - SPU_BASE) && offset <= (0x1F801DFE - SPU_BASE))
      {
        const u32 reg = (offset - (0x1F801DC0 - SPU_BASE)) / 2;
        Log_DebugPrintf("SPU reverb register %u <- 0x%04X", reg, value);
        m_tick_event->InvokeEarly();
        m_reverb_registers.rev[reg] = value;
        return;
      }

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

  const Voice& voice = m_voices[voice_index];
  if (reg_index >= 6 && voice.IsOn())
  {
    // adsr volume needs to be updated when reading
    m_tick_event->InvokeEarly();
  }

  return voice.regs.index[reg_index];
}

void SPU::WriteVoiceRegister(u32 offset, u16 value)
{
  // per-voice registers
  const u32 reg_index = (offset % 0x10);
  const u32 voice_index = (offset / 0x10);
  Assert(voice_index < 24);

  Voice& voice = m_voices[voice_index];
  if (voice.IsOn())
    m_tick_event->InvokeEarly();

  switch (reg_index)
  {
    case 0x00: // volume left
    {
      Log_DebugPrintf("SPU voice %u volume left <- 0x%04X", voice_index, value);
      voice.regs.volume_left.bits = value;
      voice.left_volume.Reset(voice.regs.volume_left);
    }
    break;

    case 0x02: // volume right
    {
      Log_DebugPrintf("SPU voice %u volume right <- 0x%04X", voice_index, value);
      voice.regs.volume_right.bits = value;
      voice.right_volume.Reset(voice.regs.volume_right);
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
    DebugAssert(m_transfer_control.mode == 2);
    std::memcpy(&m_ram[m_transfer_address], words, sizeof(u32) * word_count);
    m_transfer_address = (m_transfer_address + (sizeof(u32) * word_count)) & RAM_MASK;
  }
}

void SPU::UpdateDMARequest()
{
  const RAMTransferMode mode = m_SPUCNT.ram_transfer_mode;
  const bool request = (mode == RAMTransferMode::DMAWrite || mode == RAMTransferMode::DMARead);
  m_dma->SetRequest(DMA::Channel::SPU, true);
  m_SPUSTAT.dma_read_request = request && mode == RAMTransferMode::DMARead;
  m_SPUSTAT.dma_write_request = request && mode == RAMTransferMode::DMAWrite;
}

u16 SPU::RAMTransferRead()
{
  CheckRAMIRQ(m_transfer_address);

  u16 value;
  std::memcpy(&value, &m_ram[m_transfer_address], sizeof(value));
  m_transfer_address = (m_transfer_address + sizeof(value)) & RAM_MASK;
  return value;
}

void SPU::RAMTransferWrite(u16 value)
{
  Log_TracePrintf("SPU RAM @ 0x%08X (voice 0x%04X) <- 0x%04X", m_transfer_address,
                  m_transfer_address >> VOICE_ADDRESS_SHIFT, ZeroExtend32(value));
  DebugAssert(m_transfer_control.mode == 2);

  std::memcpy(&m_ram[m_transfer_address], &value, sizeof(value));
  m_transfer_address = (m_transfer_address + sizeof(value)) & RAM_MASK;
  CheckRAMIRQ(m_transfer_address);
}

void SPU::CheckRAMIRQ(u32 address)
{
  if (!m_SPUCNT.irq9_enable)
    return;

  if (ZeroExtend32(m_irq_address) * 8 == address)
  {
    Log_DebugPrintf("SPU IRQ at address 0x%08X", address);
    m_SPUSTAT.irq9_flag = true;
    m_interrupt_controller->InterruptRequest(InterruptController::IRQ::SPU);
  }
}

void SPU::WriteToCaptureBuffer(u32 index, s16 value)
{
  const u32 ram_address = (index * CAPTURE_BUFFER_SIZE_PER_CHANNEL) | ZeroExtend16(m_capture_buffer_position);
  // Log_DebugPrintf("write to capture buffer %u (0x%08X) <- 0x%04X", index, ram_address, u16(value));
  std::memcpy(&m_ram[ram_address], &value, sizeof(value));
  CheckRAMIRQ(ram_address);
}

void SPU::IncrementCaptureBufferPosition()
{
  m_capture_buffer_position += sizeof(s16);
  m_capture_buffer_position %= CAPTURE_BUFFER_SIZE_PER_CHANNEL;
  m_SPUSTAT.second_half_capture_buffer = m_capture_buffer_position >= (CAPTURE_BUFFER_SIZE_PER_CHANNEL / 2);
}

void SPU::Execute(TickCount ticks)
{
  u32 remaining_frames = static_cast<u32>((ticks + m_ticks_carry) / SYSCLK_TICKS_PER_SPU_TICK);
  m_ticks_carry = (ticks + m_ticks_carry) % SYSCLK_TICKS_PER_SPU_TICK;

  while (remaining_frames > 0)
  {
    AudioStream* const output_stream = m_system->GetHostInterface()->GetAudioStream();
    s16* output_frame_start;
    u32 output_frame_space;
    output_stream->BeginWrite(&output_frame_start, &output_frame_space);

    s16* output_frame = output_frame_start;
    const u32 frames_in_this_batch = std::min(remaining_frames, output_frame_space);
    for (u32 i = 0; i < frames_in_this_batch; i++)
    {
      s32 left_sum = 0;
      s32 right_sum = 0;
      s32 reverb_in_left = 0;
      s32 reverb_in_right = 0;
      if (m_SPUCNT.enable)
      {
        u32 reverb_on = m_reverb_on_register;

        for (u32 voice = 0; voice < NUM_VOICES; voice++)
        {
          const auto [left, right] = SampleVoice(voice);
          left_sum += left;
          right_sum += right;

          if (reverb_on & 1u)
          {
            reverb_in_left += left;
            reverb_in_right += right;
          }
          reverb_on >>= 1;
        }

        if (!m_SPUCNT.mute_n)
        {
          left_sum = 0;
          right_sum = 0;
        }
      }

      // Mix in CD audio.
      s16 cd_audio_left;
      s16 cd_audio_right;
      if (!m_cd_audio_buffer.IsEmpty())
      {
        cd_audio_left = m_cd_audio_buffer.Pop();
        cd_audio_right = m_cd_audio_buffer.Pop();
        if (m_SPUCNT.cd_audio_enable)
        {
          const s32 cd_audio_volume_left = ApplyVolume(s32(cd_audio_left), m_cd_audio_volume_left);
          const s32 cd_audio_volume_right = ApplyVolume(s32(cd_audio_right), m_cd_audio_volume_right);

          left_sum += cd_audio_volume_left;
          right_sum += cd_audio_volume_right;

          if (m_SPUCNT.cd_audio_reverb)
          {
            reverb_in_left += cd_audio_volume_left;
            reverb_in_right += cd_audio_volume_right;
          }
        }
      }
      else
      {
        cd_audio_left = 0;
        cd_audio_right = 0;
      }

      // Compute reverb.
      m_tick_counter++;
      if ((m_tick_counter & 1u) != 0)
      {
        m_reverb_left_input = Clamp16(reverb_in_left);
      }
      else
      {
        m_reverb_right_input = Clamp16(reverb_in_right);
        DoReverb();
      }

      // Mix in reverb.
      left_sum += m_reverb_left_output;
      right_sum += m_reverb_right_output;

      // Apply main volume before clamping.
      *(output_frame++) = Clamp16(ApplyVolume(left_sum, m_main_volume_left.current_level));
      *(output_frame++) = Clamp16(ApplyVolume(right_sum, m_main_volume_right.current_level));
      m_main_volume_left.Tick();
      m_main_volume_right.Tick();

      // Write to capture buffers.
      WriteToCaptureBuffer(0, cd_audio_left);
      WriteToCaptureBuffer(1, cd_audio_right);
      WriteToCaptureBuffer(2, Clamp16(m_voices[1].last_amplitude));
      WriteToCaptureBuffer(3, Clamp16(m_voices[3].last_amplitude));
      IncrementCaptureBufferPosition();
    }

    if (m_dump_writer)
      m_dump_writer->WriteFrames(output_frame_start, frames_in_this_batch);

    output_stream->EndWrite(frames_in_this_batch);
    remaining_frames -= frames_in_this_batch;
  }

  for (u32 i = 0; i < NUM_VOICES; i++)
  {
    const u32 delay = static_cast<u32>(m_voice_key_on_off_delay[i]);
    if (delay == 0)
      continue;

    m_voice_key_on_off_delay[i] -= static_cast<u8>(std::min(delay, static_cast<u32>(ticks)));
  }
}

void SPU::UpdateEventInterval()
{
  // Don't generate more than the audio buffer since in a single slice, otherwise we'll both overflow the buffers when
  // we do write it, and the audio thread will underflow since it won't have enough data it the game isn't messing with
  // the SPU state.
  const u32 max_slice_frames = m_system->GetHostInterface()->GetAudioStream()->GetBufferSize();

  // TODO: Make this predict how long until the interrupt will be hit instead...
  const u32 interval = (m_SPUCNT.enable && m_SPUCNT.irq9_enable) ? 1 : max_slice_frames;
  const TickCount interval_ticks = static_cast<TickCount>(interval) * SYSCLK_TICKS_PER_SPU_TICK;
  if (m_tick_event->IsActive() && m_tick_event->GetInterval() == interval_ticks)
    return;

  // Ensure all pending ticks have been executed, since we won't get them back after rescheduling.
  m_tick_event->InvokeEarly(true);
  m_tick_event->SetInterval(interval_ticks);
  m_tick_event->Schedule(interval_ticks - m_ticks_carry);
}

void SPU::GeneratePendingSamples()
{
  m_tick_event->InvokeEarly();
}

bool SPU::StartDumpingAudio(const char* filename)
{
  if (m_dump_writer)
    m_dump_writer.reset();

  m_dump_writer = std::make_unique<Common::WAVWriter>();
  if (!m_dump_writer->Open(filename, SAMPLE_RATE, 2))
  {
    Log_ErrorPrintf("Failed to open '%s'", filename);
    m_dump_writer.reset();
    return false;
  }

  return true;
}

bool SPU::StopDumpingAudio()
{
  if (!m_dump_writer)
    return false;

  m_dump_writer.reset();
  return true;
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
  if (adsr_phase == ADSRPhase::Off || adsr_phase == ADSRPhase::Release)
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

struct ADSRTableEntry
{
  s32 ticks;
  s32 step;
};
enum : u32
{
  NUM_ADSR_TABLE_ENTRIES = 128,
  NUM_ADSR_DIRECTIONS = 2 // increasing, decreasing
};
using ADSRTableEntries = std::array<std::array<ADSRTableEntry, NUM_ADSR_TABLE_ENTRIES>, NUM_ADSR_DIRECTIONS>;

static constexpr ADSRTableEntries ComputeADSRTableEntries()
{
  ADSRTableEntries entries = {};
  for (u32 decreasing = 0; decreasing < 2; decreasing++)
  {
    for (u32 rate = 0; rate < NUM_ADSR_TABLE_ENTRIES; rate++)
    {
      if (rate < 48)
      {
        entries[decreasing][rate].ticks = 1;
        if (decreasing != 0)
          entries[decreasing][rate].step =
            static_cast<s32>(static_cast<u32>(-8 + static_cast<s32>(rate & 3)) << (11 - (rate >> 2)));
        else
          entries[decreasing][rate].step = (7 - static_cast<s32>(rate & 3)) << (11 - (rate >> 2));
      }
      else
      {
        entries[decreasing][rate].ticks = 1 << (static_cast<s32>(rate >> 2) - 11);
        if (decreasing != 0)
          entries[decreasing][rate].step = (-8 + static_cast<s32>(rate & 3));
        else
          entries[decreasing][rate].step = (7 - static_cast<s32>(rate & 3));
      }
    }
  }

  return entries;
}

static constexpr ADSRTableEntries s_adsr_table = ComputeADSRTableEntries();

void SPU::VolumeEnvelope::Reset(u8 rate_, bool decreasing_, bool exponential_)
{
  rate = rate_;
  decreasing = decreasing_;
  exponential = exponential_;

  const ADSRTableEntry& table_entry = s_adsr_table[BoolToUInt8(decreasing)][rate];
  counter = table_entry.ticks;
}

s16 SPU::VolumeEnvelope::Tick(s16 current_level)
{
  counter--;
  if (counter > 0)
    return current_level;

  const ADSRTableEntry& table_entry = s_adsr_table[BoolToUInt8(decreasing)][rate];
  s32 this_step = table_entry.step;
  counter = table_entry.ticks;

  if (exponential)
  {
    if (decreasing)
    {
      this_step = (this_step * current_level) >> 15;
    }
    else
    {
      if (current_level >= 0x6000)
      {
        if (rate < 40)
        {
          this_step >>= 2;
        }
        else if (rate >= 44)
        {
          counter >>= 2;
        }
        else
        {
          this_step >>= 1;
          counter >>= 1;
        }
      }
    }
  }

  return static_cast<s16>(
    std::clamp<s32>(static_cast<s32>(current_level) + this_step, ENVELOPE_MIN_VOLUME, ENVELOPE_MAX_VOLUME));
}

void SPU::VolumeSweep::Reset(VolumeRegister reg)
{
  if (!reg.sweep_mode)
  {
    current_level = reg.fixed_volume_shr1 << 1;
    envelope_active = false;
    return;
  }

  envelope.Reset(reg.sweep_rate, reg.sweep_direction_decrease, reg.sweep_exponential);
  envelope_active = true;
}

void SPU::VolumeSweep::Tick()
{
  if (!envelope_active)
    return;

  current_level = envelope.Tick(current_level);
  envelope_active =
    (envelope.decreasing ? (current_level > ENVELOPE_MIN_VOLUME) : (current_level < ENVELOPE_MAX_VOLUME));
}

void SPU::Voice::SetADSRPhase(ADSRPhase phase)
{
  adsr_phase = phase;
  switch (phase)
  {
    case ADSRPhase::Off:
      adsr_target = 0;
      adsr_envelope.Reset(0, false, false);
      return;

    case ADSRPhase::Attack:
      adsr_target = 32767; // 0 -> max
      adsr_envelope.Reset(regs.adsr.attack_rate, false, regs.adsr.attack_exponential);
      break;

    case ADSRPhase::Decay:
      adsr_target = static_cast<s16>(std::min<s32>((u32(regs.adsr.sustain_level.GetValue()) + 1) * 0x800,
                                                   ENVELOPE_MAX_VOLUME)); // max -> sustain level
      adsr_envelope.Reset(regs.adsr.decay_rate_shr2 << 2, true, true);
      break;

    case ADSRPhase::Sustain:
      adsr_target = 0;
      adsr_envelope.Reset(regs.adsr.sustain_rate, regs.adsr.sustain_direction_decrease, regs.adsr.sustain_exponential);
      break;

    case ADSRPhase::Release:
      adsr_target = 0;
      adsr_envelope.Reset(regs.adsr.release_rate_shr2 << 2, true, regs.adsr.release_exponential);
      break;

    default:
      break;
  }
}

void SPU::Voice::TickADSR()
{
  regs.adsr_volume = adsr_envelope.Tick(regs.adsr_volume);

  if (adsr_phase != ADSRPhase::Sustain)
  {
    const bool reached_target =
      adsr_envelope.decreasing ? (regs.adsr_volume <= adsr_target) : (regs.adsr_volume >= adsr_target);
    if (reached_target)
      SetADSRPhase(GetNextADSRPhase(adsr_phase));
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
    s32 sample = s32(static_cast<s16>(ZeroExtend16(block.GetNibble(i)) << 12) >> shift);
    sample += (last_samples[0] * filter_pos) >> 6;
    sample += (last_samples[1] * filter_neg) >> 6;

    current_block_samples[i] = Clamp16(sample);
    last_samples[1] = last_samples[0];
    last_samples[0] = sample;
  }

  std::copy(last_samples, last_samples + countof(last_samples), adpcm_last_samples.begin());
  current_block_flags.bits = block.flags.bits;
}

s16 SPU::Voice::SampleBlock(s32 index) const
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
  CheckRAMIRQ(ram_address);
  CheckRAMIRQ((ram_address + 8) & RAM_MASK);

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
  {
    voice.last_amplitude = 0;
    return {};
  }

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

  // interpolate/sample and apply ADSR volume
  const s32 amplitude = ApplyVolume(voice.Interpolate(), voice.regs.adsr_volume);
  voice.last_amplitude = amplitude;
  voice.TickADSR();

  // Pitch modulation
  u16 step = voice.regs.adpcm_sample_rate;
  if (IsPitchModulationEnabled(voice_index))
  {
    const u32 factor = u32(std::clamp<s32>(m_voices[voice_index - 1].last_amplitude, -0x8000, 0x7FFF) + 0x8000);
    step = Truncate16(step * factor) >> 15;
  }
  step = std::min<u16>(step, 0x4000);

  // Shouldn't ever overflow because if sample_index == 27, step == 0x4000 there won't be a carry out from the
  // interpolation index. If there is a carry out, bit 12 will never be 1, so it'll never add more than 4 to
  // sample_index, which should never be >27.
  DebugAssert(voice.counter.sample_index < NUM_SAMPLES_PER_ADPCM_BLOCK);
  voice.counter.bits += step;

  if (voice.counter.sample_index >= NUM_SAMPLES_PER_ADPCM_BLOCK)
  {
    // next block
    voice.counter.sample_index -= NUM_SAMPLES_PER_ADPCM_BLOCK;
    voice.has_samples = false;
    voice.current_address += 2;

    // handle flags
    if (voice.current_block_flags.loop_end)
    {
      if (!voice.current_block_flags.loop_repeat)
      {
        Log_TracePrintf("Voice %u loop end+mute @ 0x%08X", voice_index, ZeroExtend32(voice.current_address));
        m_endx_register |= (u32(1) << voice_index);
        voice.regs.adsr_volume = 0;
        voice.SetADSRPhase(ADSRPhase::Off);
      }
      else
      {
        Log_TracePrintf("Voice %u loop end+repeat @ 0x%08X", voice_index, ZeroExtend32(voice.current_address));
        voice.current_address = voice.regs.adpcm_repeat_address;
      }
    }
  }

  // apply per-channel volume
  const s32 left = ApplyVolume(amplitude, voice.left_volume.current_level);
  const s32 right = ApplyVolume(amplitude, voice.right_volume.current_level);
  voice.left_volume.Tick();
  voice.right_volume.Tick();
  return std::make_tuple(left, right);
}

void SPU::VoiceKeyOn(u32 voice_index)
{
  Log_DebugPrintf("Voice %u key on", voice_index);

  if (m_voice_key_on_off_delay[voice_index] > 0)
  {
    Log_DevPrintf("Ignoring key on for voice %u due to only %u ticks passed since last write", voice_index,
                  m_voice_key_on_off_delay[voice_index]);
    return;
  }

  m_voices[voice_index].KeyOn();
  m_voice_key_on_off_delay[voice_index] = MINIMUM_TICKS_BETWEEN_KEY_ON_OFF;
}

void SPU::VoiceKeyOff(u32 voice_index)
{
  Log_DebugPrintf("Voice %u key off", voice_index);

  if (m_voice_key_on_off_delay[voice_index] > 0)
  {
    Log_DevPrintf("Ignoring key off for voice %u due to only %u ticks passed since last write", voice_index,
                  m_voice_key_on_off_delay[voice_index]);
    return;
  }

  m_voices[voice_index].KeyOff();
  m_voice_key_on_off_delay[voice_index] = MINIMUM_TICKS_BETWEEN_KEY_ON_OFF;
}

u32 SPU::ReverbMemoryAddress(u32 address) const
{
  // Ensures address does not leave the reverb work area.
  const u32 mBASE = ZeroExtend32(m_reverb_registers.mBASE) * 8;
  const u32 relative_address = (address - mBASE) % (RAM_SIZE - mBASE);
  return (mBASE + relative_address) & 0x7FFFEu;
}

s16 SPU::ReverbRead(u32 address)
{
  const u32 real_address = ReverbMemoryAddress(m_reverb_current_address + address);

  // TODO: Should this check interrupts?
  s16 data;
  std::memcpy(&data, &m_ram[real_address & RAM_MASK], sizeof(data));
  return data;
}

void SPU::ReverbWrite(u32 address, s16 data)
{
  if (!m_SPUCNT.reverb_master_enable)
    return;

  // TODO: Should this check interrupts?
  const u32 real_address = ReverbMemoryAddress(m_reverb_current_address + address);
  std::memcpy(&m_ram[real_address & RAM_MASK], &data, sizeof(data));
}

// Implements saturated add, subtract and multiply for reverb computations.
struct ReverbSample
{
  s16 value;

  static ALWAYS_INLINE s16 OpAdd(s16 lhs, s16 rhs)
  {
    s32 result = s32(lhs) + s32(rhs);
    return s16((result < -32768) ? -32768 : ((result > 32767) ? 32767 : result));
  }

  static ALWAYS_INLINE s16 OpSub(s16 lhs, s16 rhs)
  {
    s32 result = s32(lhs) - s32(rhs);
    return s16((result < -32768) ? -32768 : ((result > 32767) ? 32767 : result));
  }

  static ALWAYS_INLINE s16 OpMul(s16 lhs, s16 rhs) { return s16((s32(lhs) * s32(rhs)) >> 15); }

  ALWAYS_INLINE ReverbSample operator+(ReverbSample rhs) const { return ReverbSample{OpAdd(value, rhs.value)}; }

  ALWAYS_INLINE ReverbSample operator-(ReverbSample rhs) const { return ReverbSample{OpSub(value, rhs.value)}; }

  ALWAYS_INLINE ReverbSample operator*(ReverbSample rhs) const { return ReverbSample{OpMul(value, rhs.value)}; }

  ALWAYS_INLINE ReverbSample& operator+=(ReverbSample rhs)
  {
    value = OpAdd(value, rhs.value);
    return *this;
  }

  ALWAYS_INLINE ReverbSample& operator-=(ReverbSample rhs)
  {
    value = OpSub(value, rhs.value);
    return *this;
  }

  ALWAYS_INLINE ReverbSample& operator*=(ReverbSample rhs)
  {
    value = OpMul(value, rhs.value);
    return *this;
  }
};

void SPU::DoReverb()
{
  const ReverbSample Lin(ReverbSample{m_reverb_left_input} * ReverbSample{m_reverb_registers.vLIN});
  const ReverbSample Rin(ReverbSample{m_reverb_right_input} * ReverbSample{m_reverb_registers.vRIN});

#define R(name)                                                                                                        \
  ReverbSample { m_reverb_registers.name }
#define Rm(name) (u32(m_reverb_registers.name) * 8u)
#define MR(addr)                                                                                                       \
  ReverbSample { ReverbRead(addr) }
#define MW(addr, value_) ReverbWrite((addr), (value_).value)

  // [mLSAME] = (Lin + [dLSAME]*vWALL - [mLSAME-2])*vIIR + [mLSAME-2]  ;L-to-L
  MW(Rm(mLSAME), ((Lin + (MR(Rm(dLSAME)) * R(vWALL)) - MR(Rm(mLSAME) - 2)) * R(vIIR)) + MR(Rm(mLSAME) - 2));

  // [mRSAME] = (Rin + [dRSAME]*vWALL - [mRSAME-2])*vIIR + [mRSAME-2]  ;R-to-R
  MW(Rm(mLSAME), ((Rin + (MR(Rm(dRSAME)) * R(vWALL)) - MR(Rm(mRSAME) - 2)) * R(vIIR)) + MR(Rm(mRSAME) - 2));

  // [mLDIFF] = (Lin + [dRDIFF]*vWALL - [mLDIFF-2])*vIIR + [mLDIFF-2]  ;R-to-L
  MW(Rm(mLDIFF), ((Lin + (MR(Rm(dRDIFF)) * R(vWALL)) - MR(Rm(mLDIFF) - 2)) * R(vIIR)) + MR(Rm(mLDIFF) - 2));

  // [mRDIFF] = (Rin + [dLDIFF]*vWALL - [mRDIFF-2])*vIIR + [mRDIFF-2]  ;L-to-R
  MW(Rm(mRDIFF), ((Rin + (MR(Rm(dLDIFF)) * R(vWALL)) - MR(Rm(mRDIFF) - 2)) * R(vIIR)) + MR(Rm(mRDIFF) - 2));

  // Lout = vCOMB1 * [mLCOMB1] + vCOMB2 * [mLCOMB2] + vCOMB3 * [mLCOMB3] + vCOMB4 * [mLCOMB4]
  ReverbSample Lout{(R(vCOMB1) * MR(Rm(mLCOMB1))) + (R(vCOMB2) * MR(Rm(mLCOMB2))) + (R(vCOMB3) * MR(Rm(mLCOMB3))) +
                    (R(vCOMB4) * MR(Rm(mLCOMB4)))};

  // Rout = vCOMB1 * [mRCOMB1] + vCOMB2 * [mRCOMB2] + vCOMB3 * [mRCOMB3] + vCOMB4 * [mRCOMB4]
  ReverbSample Rout{(R(vCOMB1) * MR(Rm(mRCOMB1))) + (R(vCOMB2) * MR(Rm(mRCOMB2))) + (R(vCOMB3) * MR(Rm(mRCOMB3))) +
                    (R(vCOMB4) * MR(Rm(mRCOMB4)))};

  // Lout = Lout - vAPF1 * [mLAPF1 - dAPF1], [mLAPF1] = Lout, Lout = Lout * vAPF1 + [mLAPF1 - dAPF1]
  Lout = Lout - (R(vAPF1) * MR(Rm(mLAPF1) - Rm(dAPF1)));
  MW(Rm(mLAPF1), Lout);
  Lout = (Lout * R(vAPF1)) + MR(Rm(mLAPF1) - Rm(dAPF1));

  // Rout = Rout - vAPF1 * [mRAPF1 - dAPF1], [mRAPF1] = Rout, Rout = Rout * vAPF1 + [mRAPF1 - dAPF1]
  Rout = Rout - (R(vAPF1) * MR(Rm(mRAPF1) - Rm(dAPF1)));
  MW(Rm(mRAPF1), Rout);
  Rout = (Rout * R(vAPF1)) + MR(Rm(mRAPF1) - Rm(dAPF1));

  // Lout = Lout - vAPF2 * [mLAPF2 - dAPF2], [mLAPF2] = Lout, Lout = Lout * vAPF2 + [mLAPF2 - dAPF2]
  Lout = Lout - (R(vAPF2) * MR(Rm(mLAPF2) - Rm(dAPF2)));
  MW(Rm(mLAPF2), Lout);
  Lout = (Lout * R(vAPF2)) + MR(Rm(mLAPF2) - Rm(dAPF2));

  // Rout = Rout - vAPF2 * [mRAPF2 - dAPF2], [mRAPF2] = Rout, Rout = Rout * vAPF2 + [mRAPF2 - dAPF2]
  Rout = Rout - (R(vAPF2) * MR(Rm(mRAPF2) - Rm(dAPF2)));
  MW(Rm(mRAPF2), Rout);
  Rout = (Rout * R(vAPF2)) + MR(Rm(mRAPF2) - Rm(dAPF2));

  // LeftOutput  = Lout*vLOUT
  m_reverb_left_output = (Lout * R(vLOUT)).value;

  // RightOutput = Rout*vROUT
  m_reverb_right_output = (Rout * R(vROUT)).value;

  // BufferAddress = MAX(mBASE, (BufferAddress+2) AND 7FFFEh)
  m_reverb_current_address = ReverbMemoryAddress(m_reverb_current_address + 2);

#undef MW
#undef MR
#undef Rm
#undef R
}

void SPU::EnsureCDAudioSpace(u32 remaining_frames)
{
  if (m_cd_audio_buffer.GetSpace() >= (remaining_frames * 2))
    return;

  const u32 frames_to_drop = (remaining_frames * 2) - m_cd_audio_buffer.GetSpace();
  Log_WarningPrintf(
    "SPU CD Audio buffer overflow with %d pending ticks - writing %u frames with %u frames space. Dropping %u frames.",
    m_tick_event->IsActive() ? (m_tick_event->GetTicksSinceLastExecution() / SYSCLK_TICKS_PER_SPU_TICK) : 0,
    remaining_frames, m_cd_audio_buffer.GetSpace() / 2, frames_to_drop);
  m_cd_audio_buffer.Remove(frames_to_drop);
}

void SPU::DrawDebugStateWindow()
{
  static const ImVec4 active_color{1.0f, 1.0f, 1.0f, 1.0f};
  static const ImVec4 inactive_color{0.4f, 0.4f, 0.4f, 1.0f};
  const float framebuffer_scale = ImGui::GetIO().DisplayFramebufferScale.x;

  ImGui::SetNextWindowSize(ImVec2(800.0f * framebuffer_scale, 600.0f * framebuffer_scale), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("SPU State", &m_system->GetSettings().debugging.show_spu_state))
  {
    ImGui::End();
    return;
  }

  // status
  if (ImGui::CollapsingHeader("Status", ImGuiTreeNodeFlags_DefaultOpen))
  {
    static constexpr std::array<const char*, 4> transfer_modes = {
      {"Transfer Stopped", "Manual Write", "DMA Write", "DMA Read"}};
    const std::array<float, 6> offsets = {{100.0f * framebuffer_scale, 200.0f * framebuffer_scale,
                                           300.0f * framebuffer_scale, 420.0f * framebuffer_scale,
                                           500.0f * framebuffer_scale, 600.0f * framebuffer_scale}};

    ImGui::Text("Control: ");
    ImGui::SameLine(offsets[0]);
    ImGui::TextColored(m_SPUCNT.enable ? active_color : inactive_color, "SPU Enable");
    ImGui::SameLine(offsets[1]);
    ImGui::TextColored(m_SPUCNT.mute_n ? inactive_color : active_color, "Mute SPU");
    ImGui::SameLine(offsets[2]);
    ImGui::TextColored(m_SPUCNT.external_audio_enable ? active_color : inactive_color, "External Audio");
    ImGui::SameLine(offsets[3]);
    ImGui::TextColored(m_SPUCNT.ram_transfer_mode != RAMTransferMode::Stopped ? active_color : inactive_color, "%s",
                       transfer_modes[static_cast<u8>(m_SPUCNT.ram_transfer_mode.GetValue())]);

    ImGui::Text("Status: ");
    ImGui::SameLine(offsets[0]);
    ImGui::TextColored(m_SPUSTAT.irq9_flag ? active_color : inactive_color, "IRQ9");
    ImGui::SameLine(offsets[1]);
    ImGui::TextColored(m_SPUSTAT.dma_read_write_request ? active_color : inactive_color, "DMA Request");
    ImGui::SameLine(offsets[2]);
    ImGui::TextColored(m_SPUSTAT.dma_read_request ? active_color : inactive_color, "DMA Read");
    ImGui::SameLine(offsets[3]);
    ImGui::TextColored(m_SPUSTAT.dma_write_request ? active_color : inactive_color, "DMA Write");
    ImGui::SameLine(offsets[4]);
    ImGui::TextColored(m_SPUSTAT.transfer_busy ? active_color : inactive_color, "Transfer Busy");
    ImGui::SameLine(offsets[5]);
    ImGui::TextColored(m_SPUSTAT.second_half_capture_buffer ? active_color : inactive_color, "Second Capture Buffer");

    ImGui::Text("Interrupt: ");
    ImGui::SameLine(offsets[0]);
    ImGui::TextColored(m_SPUCNT.irq9_enable ? active_color : inactive_color,
                       m_SPUCNT.irq9_enable ? "Enabled @ 0x%04X (actual 0x%08X)" : "Disabled @ 0x%04X (actual 0x%08X)",
                       m_irq_address, (ZeroExtend32(m_irq_address) * 8) & RAM_MASK);

    ImGui::Text("Volume: ");
    ImGui::SameLine(offsets[0]);
    ImGui::Text("Left: %d%%", ApplyVolume(100, m_main_volume_left.current_level));
    ImGui::SameLine(offsets[1]);
    ImGui::Text("Right: %d%%", ApplyVolume(100, m_main_volume_right.current_level));

    ImGui::Text("CD Audio: ");
    ImGui::SameLine(offsets[0]);
    ImGui::TextColored(m_SPUCNT.cd_audio_enable ? active_color : inactive_color,
                       m_SPUCNT.cd_audio_enable ? "Enabled" : "Disabled");
    ImGui::SameLine(offsets[1]);
    ImGui::TextColored(m_SPUCNT.cd_audio_enable ? active_color : inactive_color, "Left Volume: %d%%",
                       ApplyVolume(100, m_cd_audio_volume_left));
    ImGui::SameLine(offsets[3]);
    ImGui::TextColored(m_SPUCNT.cd_audio_enable ? active_color : inactive_color, "Right Volume: %d%%",
                       ApplyVolume(100, m_cd_audio_volume_left));
  }

  // draw voice states
  if (ImGui::CollapsingHeader("Voice State", ImGuiTreeNodeFlags_DefaultOpen))
  {
    static constexpr u32 NUM_COLUMNS = 12;

    ImGui::Columns(NUM_COLUMNS);

    // headers
    static constexpr std::array<const char*, NUM_COLUMNS> column_titles = {
      {"#", "InterpIndex", "SampleIndex", "CurAddr", "StartAddr", "RepeatAddr", "SampleRate", "VolLeft", "VolRight",
       "ADSRPhase", "ADSRVol", "ADSRTicks"}};
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
      ImGui::TextColored(color, "%.2f", (float(v.regs.adpcm_sample_rate) / 4096.0f) * 44100.0f);
      ImGui::NextColumn();
      ImGui::TextColored(color, "%d%%", ApplyVolume(100, v.left_volume.current_level));
      ImGui::NextColumn();
      ImGui::TextColored(color, "%d%%", ApplyVolume(100, v.right_volume.current_level));
      ImGui::NextColumn();
      ImGui::TextColored(color, "%s", adsr_phases[static_cast<u8>(v.adsr_phase)]);
      ImGui::NextColumn();
      ImGui::TextColored(color, "%d%%", ApplyVolume(100, v.regs.adsr_volume));
      ImGui::NextColumn();
      ImGui::TextColored(color, "%d", v.adsr_envelope.counter);
      ImGui::NextColumn();
    }

    ImGui::Columns(1);
  }

  if (ImGui::CollapsingHeader("Reverb", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImGui::TextColored(m_SPUCNT.reverb_master_enable ? active_color : inactive_color, "Master Enable: %s",
                       m_SPUCNT.reverb_master_enable ? "Yes" : "No");
    ImGui::Text("Voices Enabled: ");

    for (u32 i = 0; i < NUM_VOICES; i++)
    {
      ImGui::SameLine(0.0f, 16.0f);

      const bool active = IsVoiceReverbEnabled(i);
      ImGui::TextColored(active ? active_color : inactive_color, "%u", i);
    }

    ImGui::TextColored(m_SPUCNT.cd_audio_reverb ? active_color : inactive_color, "CD Audio Enable: %s",
                       m_SPUCNT.cd_audio_reverb ? "Yes" : "No");

    ImGui::TextColored(m_SPUCNT.external_audio_reverb ? active_color : inactive_color, "External Audio Enable: %s",
                       m_SPUCNT.external_audio_reverb ? "Yes" : "No");

    ImGui::Text("Current Address: 0x%08X", m_reverb_current_address);
    ImGui::Text("Current Volume: Input (%d, %d) Output (%d, %d)", m_reverb_left_input, m_reverb_right_input,
                m_reverb_left_output, m_reverb_right_output);

    ImGui::Text("Pitch Modulation: ");
    for (u32 i = 1; i < NUM_VOICES; i++)
    {
      ImGui::SameLine(0.0f, 16.0f);

      const bool active = IsPitchModulationEnabled(i);
      ImGui::TextColored(active ? active_color : inactive_color, "%u", i);
    }
  }

  ImGui::End();
}
