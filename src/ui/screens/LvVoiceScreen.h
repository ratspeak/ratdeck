#pragma once

#include "ui/UIManager.h"
#include <functional>

// =============================================================================
// Voice app — full-screen (tab bar hidden) Voice+Codec2 decode/play UI.
//
// Plus has no mic, so this screen is decode/play only. Two actions:
//   PLAY — play /Files/voice/probe.wav (or probe_c2.wav if probe.wav missing).
//   C2   — round-trip probe.wav → probe.c2 → probe_c2.wav then play the
//          decoded WAV. If only probe.c2 exists on SD, just decode that.
//
// BACK → Apps hub.
// =============================================================================

class AudioNotify;

class LvVoiceScreen : public LvScreen {
public:
    void createUI(lv_obj_t* parent) override;
    void destroyUI() override;
    void refreshUI() override;
    void onEnter() override;
    void onExit() override;
    bool handleKey(const KeyEvent& event) override;

    void setAudio(AudioNotify* audio) { _audio = audio; }
    void setUIManager(class UIManager* ui) { _ui = ui; }

    using BackCallback = std::function<void()>;
    void setBackCallback(BackCallback cb) { _onBack = cb; }

    const char* title() const override { return "Voice"; }

private:
    void rebuild();
    void doPlay();
    void doC2();

    class UIManager* _ui = nullptr;
    AudioNotify* _audio = nullptr;
    BackCallback _onBack;

    lv_obj_t* _backBtn = nullptr;
    lv_obj_t* _title = nullptr;
    lv_obj_t* _body = nullptr;
    lv_obj_t* _playBtn = nullptr;
    lv_obj_t* _c2Btn = nullptr;
};
