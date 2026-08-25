#include "LvVoiceScreen.h"
#include "audio/AudioNotify.h"
#include "ui/LvTheme.h"
#include "ui/Theme.h"
#include "fonts/fonts.h"
#include "util/Codec2Voice.h"
#include <SD.h>
#include <stdio.h>

namespace {

constexpr lv_coord_t kBackX = 4;
constexpr lv_coord_t kBackY = 2;
constexpr lv_coord_t kBackW = 64;
constexpr lv_coord_t kBackH = 18;
constexpr lv_coord_t kBodyY = 28;
constexpr lv_coord_t kAppContentH = Theme::SCREEN_H - Theme::STATUS_BAR_H;  // 220
constexpr lv_coord_t kBtnH = 28;
constexpr lv_coord_t kBtnY = kAppContentH - kBtnH - 6;

// Helper: human-readable size, "—" for missing files.
const char* sizeStr(uint64_t sz) {
  static char buf[24];
  if (sz == 0) return "—";
  if (sz < 1024) {
    snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)sz);
  } else {
    snprintf(buf, sizeof(buf), "%llu KB", (unsigned long long)(sz / 1024));
  }
  return buf;
}

uint64_t fileSize(const char* path) {
  if (!SD.exists(path)) return 0;
  File f = SD.open(path, FILE_READ);
  if (!f) return 0;
  uint64_t sz = f.size();
  f.close();
  return sz;
}

}  // namespace

void LvVoiceScreen::createUI(lv_obj_t* parent) {
    _screen = parent;
    lv_obj_set_style_bg_color(parent, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_style_radius(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    _backBtn = lv_btn_create(parent);
    lv_obj_set_size(_backBtn, kBackW, kBackH);
    lv_obj_set_pos(_backBtn, kBackX, kBackY);
    lv_obj_add_style(_backBtn, LvTheme::styleBtn(), 0);
    lv_obj_add_style(_backBtn, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(_backBtn, LV_OPA_70, 0);
    lv_obj_set_style_border_color(_backBtn, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(_backBtn, 1, 0);
    lv_obj_set_style_radius(_backBtn, 3, 0);
    lv_obj_set_style_pad_all(_backBtn, 0, 0);
    lv_obj_set_style_shadow_width(_backBtn, 0, 0);
    lv_obj_t* backLbl = lv_label_create(_backBtn);
    lv_obj_set_style_text_font(backLbl, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(backLbl, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_text(backLbl, "< BACK");
    lv_obj_center(backLbl);
    lv_obj_add_event_cb(_backBtn, [](lv_event_t* e) {
        auto* self = (LvVoiceScreen*)lv_event_get_user_data(e);
        if (self->_onBack) self->_onBack();
    }, LV_EVENT_CLICKED, this);

    _title = lv_label_create(parent);
    lv_obj_set_style_text_font(_title, &lv_font_rsdeck_12, 0);
    lv_obj_set_style_text_color(_title, lv_color_hex(Theme::ACCENT), 0);
    lv_obj_set_style_text_letter_space(_title, 1, 0);
    lv_label_set_text(_title, "VOICE");
    lv_obj_set_pos(_title, kBackX + kBackW + 8, kBackY + 1);

    _body = lv_label_create(parent);
    lv_obj_set_style_text_font(_body, &lv_font_rsdeck_12, 0);
    lv_obj_set_style_text_color(_body, lv_color_hex(Theme::TEXT_PRIMARY), 0);
    lv_label_set_long_mode(_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_body, Theme::CONTENT_W - 16);
    lv_obj_set_pos(_body, 8, kBodyY);
    lv_label_set_text(_body, "Loading…");

    _playBtn = lv_btn_create(parent);
    lv_obj_set_size(_playBtn, 100, kBtnH);
    lv_obj_set_pos(_playBtn, 8, kBtnY);
    lv_obj_add_style(_playBtn, LvTheme::styleBtn(), 0);
    lv_obj_add_style(_playBtn, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(_playBtn, LV_OPA_70, 0);
    lv_obj_set_style_border_color(_playBtn, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(_playBtn, 1, 0);
    lv_obj_set_style_radius(_playBtn, 3, 0);
    lv_obj_set_style_pad_all(_playBtn, 0, 0);
    lv_obj_set_style_shadow_width(_playBtn, 0, 0);
    lv_obj_t* pLbl = lv_label_create(_playBtn);
    lv_obj_set_style_text_font(pLbl, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(pLbl, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_text(pLbl, "PLAY");
    lv_obj_center(pLbl);
    lv_obj_add_event_cb(_playBtn, [](lv_event_t* e) {
        auto* self = (LvVoiceScreen*)lv_event_get_user_data(e);
        self->doPlay();
    }, LV_EVENT_CLICKED, this);

    _c2Btn = lv_btn_create(parent);
    lv_obj_set_size(_c2Btn, 110, kBtnH);
    lv_obj_set_pos(_c2Btn, Theme::CONTENT_W - 118, kBtnY);
    lv_obj_add_style(_c2Btn, LvTheme::styleBtn(), 0);
    lv_obj_add_style(_c2Btn, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(_c2Btn, LV_OPA_70, 0);
    lv_obj_set_style_border_color(_c2Btn, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(_c2Btn, 1, 0);
    lv_obj_set_style_radius(_c2Btn, 3, 0);
    lv_obj_set_style_pad_all(_c2Btn, 0, 0);
    lv_obj_set_style_shadow_width(_c2Btn, 0, 0);
    lv_obj_t* cLbl = lv_label_create(_c2Btn);
    lv_obj_set_style_text_font(cLbl, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(cLbl, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_text(cLbl, "C2 ROUNDTRIP");
    lv_obj_center(cLbl);
    lv_obj_add_event_cb(_c2Btn, [](lv_event_t* e) {
        auto* self = (LvVoiceScreen*)lv_event_get_user_data(e);
        self->doC2();
    }, LV_EVENT_CLICKED, this);
}

void LvVoiceScreen::destroyUI() {
    _backBtn = nullptr;
    _title = nullptr;
    _body = nullptr;
    _playBtn = nullptr;
    _c2Btn = nullptr;
    LvScreen::destroyUI();
}

void LvVoiceScreen::onEnter() {
    rebuild();
}

void LvVoiceScreen::onExit() {
}

void LvVoiceScreen::refreshUI() {
    rebuild();
}

void LvVoiceScreen::rebuild() {
    if (!_body) return;

    const uint64_t wavSz = fileSize("/Files/voice/probe.wav");
    const uint64_t c2Sz = fileSize("/Files/voice/probe.c2");
    const uint64_t c2WavSz = fileSize("/Files/voice/probe_c2.wav");

    char buf[640];
    snprintf(buf, sizeof(buf),
             "Mode 1300 (1.3 kbps)\n"
             "Decode/play only (no mic)\n"
             "\n"
             "probe.wav     %s\n"
             "probe.c2      %s\n"
             "probe_c2.wav  %s",
             sizeStr(wavSz), sizeStr(c2Sz), sizeStr(c2WavSz));
    lv_label_set_text(_body, buf);
}

void LvVoiceScreen::doPlay() {
    if (!_audio) {
        if (_ui) _ui->lvStatusBar().showToast("audio not ready", 1200);
        return;
    }
    if (_audio->wavExists("/Files/voice/probe.wav")) {
        if (!_audio->playWav("/Files/voice/probe.wav")) {
            if (_ui) _ui->lvStatusBar().showToast("play failed", 1200);
        }
        return;
    }
    if (_audio->wavExists("/Files/voice/probe_c2.wav")) {
        if (!_audio->playWav("/Files/voice/probe_c2.wav")) {
            if (_ui) _ui->lvStatusBar().showToast("play failed", 1200);
        }
        return;
    }
    if (_ui) _ui->lvStatusBar().showToast("no wav — copy probe.wav to SD", 2000);
}

void LvVoiceScreen::doC2() {
    if (!_audio) {
        if (_ui) _ui->lvStatusBar().showToast("audio not ready", 1200);
        return;
    }
    if (Codec2Voice::c2Exists()) {
        // Decode existing .c2 → probe_c2.wav → play.
        Codec2Voice::Result r = Codec2Voice::decodeC2ToWav();
        if (!r.ok) {
            if (_ui) _ui->lvStatusBar().showToast("c2 decode fail", 1500);
            return;
        }
        if (!_audio->playWav("/Files/voice/probe_c2.wav")) {
            if (_ui) _ui->lvStatusBar().showToast("play failed", 1200);
        }
        rebuild();
        return;
    }
    if (!_audio->wavExists("/Files/voice/probe.wav")) {
        if (_ui) _ui->lvStatusBar().showToast("need probe.wav or probe.c2", 2000);
        return;
    }
    // Round-trip: encode probe.wav → probe.c2 → probe_c2.wav, then play.
    Codec2Voice::Result r = Codec2Voice::roundTripFiles(
        "/Files/voice/probe.wav", Codec2Voice::kProbeC2Path,
        Codec2Voice::kProbeC2WavPath);
    if (!r.ok) {
        if (_ui) _ui->lvStatusBar().showToast("c2 round-trip fail", 1500);
        return;
    }
    if (!_audio->playWav("/Files/voice/probe_c2.wav")) {
        if (_ui) _ui->lvStatusBar().showToast("play failed", 1200);
    }
    rebuild();
}

bool LvVoiceScreen::handleKey(const KeyEvent& event) {
    if (event.character == 0x1B || event.del || event.character == 0x08) {
        if (_onBack) {
            _onBack();
            return true;
        }
    }
    return false;
}
