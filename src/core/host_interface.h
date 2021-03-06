#pragma once
#include "common/string.h"
#include "common/timer.h"
#include "settings.h"
#include "types.h"
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class AudioStream;
class ByteStream;
class CDImage;
class HostDisplay;
class GameList;

class System;
struct SystemBootParameters;

class HostInterface
{
  friend System;

public:
  HostInterface();
  virtual ~HostInterface();

  /// Access to host display.
  ALWAYS_INLINE HostDisplay* GetDisplay() const { return m_display; }

  /// Access to host audio stream.
  ALWAYS_INLINE AudioStream* GetAudioStream() const { return m_audio_stream.get(); }

  /// Returns a settings object which can be modified.
  ALWAYS_INLINE Settings& GetSettings() { return m_settings; }

  /// Returns the game list.
  ALWAYS_INLINE const GameList* GetGameList() const { return m_game_list.get(); }

  /// Access to emulated system.
  ALWAYS_INLINE System* GetSystem() const { return m_system.get(); }

  bool BootSystem(const SystemBootParameters& parameters);
  void PauseSystem(bool paused);
  void ResetSystem();
  void PowerOffSystem();
  void DestroySystem();

  /// Loads state from the specified filename.
  bool LoadState(const char* filename);

  /// Loads the current emulation state from file. Specifying a slot of -1 loads the "resume" game state.
  bool LoadState(bool global, s32 slot);

  /// Saves the current emulation state to a file. Specifying a slot of -1 saves the "resume" save state.
  bool SaveState(bool global, s32 slot);

  /// Loads the resume save state for the given game. Optionally boots the game anyway if loading fails.
  bool ResumeSystemFromState(const char* filename, bool boot_on_failure);

  /// Loads the most recent resume save state. This may be global or per-game.
  bool ResumeSystemFromMostRecentState();

  /// Saves the resume save state, call when shutting down. Not called automatically on DestroySystem() since that can
  /// be called as a result of an error.
  bool SaveResumeSaveState();

  /// Returns true if currently dumping audio.
  bool IsDumpingAudio() const;

  /// Starts dumping audio to a file. If no file name is provided, one will be generated automatically.
  bool StartDumpingAudio(const char* filename = nullptr);

  /// Stops dumping audio to file if it has been started.
  void StopDumpingAudio();

  /// Saves a screenshot to the specified file. IF no file name is provided, one will be generated automatically.
  bool SaveScreenshot(const char* filename = nullptr, bool full_resolution = true, bool apply_aspect_ratio = true);

  virtual void ReportError(const char* message);
  virtual void ReportMessage(const char* message);
  virtual bool ConfirmMessage(const char* message);

  void ReportFormattedError(const char* format, ...);
  void ReportFormattedMessage(const char* format, ...);
  bool ConfirmFormattedMessage(const char* format, ...);

  /// Adds OSD messages, duration is in seconds.
  void AddOSDMessage(const char* message, float duration = 2.0f);
  void AddFormattedOSDMessage(float duration, const char* format, ...);

  /// Returns the base user directory path.
  ALWAYS_INLINE const std::string& GetUserDirectory() const { return m_user_directory; }

  /// Returns a path relative to the user directory.
  std::string GetUserDirectoryRelativePath(const char* format, ...) const;

  /// Returns a string which can be used as part of a filename, based on the current date/time.
  static TinyString GetTimestampStringForFileName();

  /// Displays a loading screen with the logo, rendered with ImGui. Use when executing possibly-time-consuming tasks
  /// such as compiling shaders when starting up.
  void DisplayLoadingScreen(const char* message, int progress_min = -1, int progress_max = -1, int progress_value = -1);

  /// Deletes save states for the specified game code. If resume is set, the resume state is deleted too.
  void DeleteSaveStates(const char* game_code, bool resume);

protected:
  enum : u32
  {
    SETTINGS_VERSION = 2,
    AUDIO_SAMPLE_RATE = 44100,
    AUDIO_CHANNELS = 2,
    AUDIO_BUFFER_SIZE = 2048,
    AUDIO_BUFFERS = 2,
    PER_GAME_SAVE_STATE_SLOTS = 10,
    GLOBAL_SAVE_STATE_SLOTS = 10,
  };

  struct OSDMessage
  {
    std::string text;
    Common::Timer time;
    float duration;
  };

  struct SaveStateInfo
  {
    std::string path;
    u64 timestamp;
    s32 slot;
    bool global;
  };

  virtual bool AcquireHostDisplay() = 0;
  virtual void ReleaseHostDisplay() = 0;
  virtual std::unique_ptr<AudioStream> CreateAudioStream(AudioBackend backend) = 0;

  virtual void OnSystemCreated();
  virtual void OnSystemPaused(bool paused);
  virtual void OnSystemDestroyed();
  virtual void OnSystemPerformanceCountersUpdated();
  virtual void OnSystemStateSaved(bool global, s32 slot);
  virtual void OnRunningGameChanged();
  virtual void OnControllerTypeChanged(u32 slot);
  virtual void DrawImGuiWindows();

  void SetUserDirectory();

  /// Ensures all subdirectories of the user directory are created.
  void CreateUserDirectorySubdirectories();

  /// Returns the path of the settings file.
  std::string GetSettingsFileName() const;

  /// Returns the path of the game list cache file.
  std::string GetGameListCacheFileName() const;

  /// Returns the path of the game database cache file.
  std::string GetGameListDatabaseFileName() const;

  /// Returns the path to a save state file. Specifying an index of -1 is the "resume" save state.
  std::string GetGameSaveStateFileName(const char* game_code, s32 slot) const;

  /// Returns the path to a save state file. Specifying an index of -1 is the "resume" save state.
  std::string GetGlobalSaveStateFileName(s32 slot) const;

  /// Returns the default path to a memory card.
  std::string GetSharedMemoryCardPath(u32 slot) const;

  /// Returns the default path to a memory card for a specific game.
  std::string GetGameMemoryCardPath(const char* game_code, u32 slot) const;

  /// Returns a list of save states for the specified game code.
  std::vector<SaveStateInfo> GetAvailableSaveStates(const char* game_code) const;

  /// Returns save state info if present. If game_code is null or empty, assumes global state.
  std::optional<SaveStateInfo> GetSaveStateInfo(const char* game_code, s32 slot);

  /// Returns the most recent resume save state.
  std::string GetMostRecentResumeSaveStatePath() const;

  /// Loads the BIOS image for the specified region.
  std::optional<std::vector<u8>> GetBIOSImage(ConsoleRegion region);

  /// Ensures the settings is valid and the correct version. If not, resets to defaults.
  void CheckSettings(SettingsInterface& si);

  /// Restores all settings to defaults.
  virtual void SetDefaultSettings(SettingsInterface& si);

  /// Applies new settings, updating internal state as needed. apply_callback should call m_settings.Load() after
  /// locking any required mutexes.
  void UpdateSettings(const std::function<void()>& apply_callback);

  /// Quick switch between software and hardware rendering.
  void ToggleSoftwareRendering();

  /// Adjusts the internal (render) resolution of the hardware backends.
  void ModifyResolutionScale(s32 increment);

  /// Switches the GPU renderer by saving state, recreating the display window, and restoring state (if needed).
  void RecreateSystem();

  /// Increases timer resolution when supported by the host OS.
  void SetTimerResolutionIncreased(bool enabled);

  void UpdateSpeedLimiterState();

  void DrawFPSWindow();
  void DrawOSDMessages();
  void DrawDebugWindows();

  HostDisplay* m_display = nullptr;
  std::unique_ptr<AudioStream> m_audio_stream;
  std::unique_ptr<System> m_system;
  std::unique_ptr<GameList> m_game_list;
  Settings m_settings;
  std::string m_user_directory;

  bool m_paused = false;
  bool m_speed_limiter_temp_disabled = false;
  bool m_speed_limiter_enabled = false;
  bool m_timer_resolution_increased = false;

  std::deque<OSDMessage> m_osd_messages;
  std::mutex m_osd_messages_lock;

private:
  void CreateAudioStream();
  bool SaveState(const char* filename);
};
