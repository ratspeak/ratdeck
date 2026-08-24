#include "LvGpsScreen.h"
#include "hal/GPSManager.h"
#include "ui/LvTheme.h"
#include "ui/Theme.h"
#include "fonts/fonts.h"
#include "util/CoordFormat.h"

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

}  // namespace

void LvGpsScreen::createUI(lv_obj_t* parent) {
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
        auto* self = (LvGpsScreen*)lv_event_get_user_data(e);
        if (self->_onBack) self->_onBack();
    }, LV_EVENT_CLICKED, this);

    _title = lv_label_create(parent);
    lv_obj_set_style_text_font(_title, &lv_font_rsdeck_12, 0);
    lv_obj_set_style_text_color(_title, lv_color_hex(Theme::ACCENT), 0);
    lv_obj_set_style_text_letter_space(_title, 1, 0);
    lv_label_set_text(_title, "GPS");
    lv_obj_set_pos(_title, kBackX + kBackW + 8, kBackY + 1);

    _body = lv_label_create(parent);
    lv_obj_set_style_text_font(_body, &lv_font_rsdeck_12, 0);
    lv_obj_set_style_text_color(_body, lv_color_hex(Theme::TEXT_PRIMARY), 0);
    lv_label_set_long_mode(_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_body, Theme::CONTENT_W - 16);
    lv_obj_set_pos(_body, 8, kBodyY);
    lv_label_set_text(_body, "Loading…");

    _refreshBtn = lv_btn_create(parent);
    lv_obj_set_size(_refreshBtn, 100, kBtnH);
    lv_obj_set_pos(_refreshBtn, 8, kBtnY);
    lv_obj_add_style(_refreshBtn, LvTheme::styleBtn(), 0);
    lv_obj_add_style(_refreshBtn, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(_refreshBtn, LV_OPA_70, 0);
    lv_obj_set_style_border_color(_refreshBtn, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(_refreshBtn, 1, 0);
    lv_obj_set_style_radius(_refreshBtn, 3, 0);
    lv_obj_set_style_pad_all(_refreshBtn, 0, 0);
    lv_obj_set_style_shadow_width(_refreshBtn, 0, 0);
    lv_obj_t* rLbl = lv_label_create(_refreshBtn);
    lv_obj_set_style_text_font(rLbl, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(rLbl, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_text(rLbl, "REFRESH");
    lv_obj_center(rLbl);
    lv_obj_add_event_cb(_refreshBtn, [](lv_event_t* e) {
        auto* self = (LvGpsScreen*)lv_event_get_user_data(e);
        self->rebuild();
    }, LV_EVENT_CLICKED, this);

    _mapBtn = lv_btn_create(parent);
    lv_obj_set_size(_mapBtn, 110, kBtnH);
    lv_obj_set_pos(_mapBtn, Theme::CONTENT_W - 118, kBtnY);
    lv_obj_add_style(_mapBtn, LvTheme::styleBtn(), 0);
    lv_obj_add_style(_mapBtn, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(_mapBtn, LV_OPA_70, 0);
    lv_obj_set_style_border_color(_mapBtn, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(_mapBtn, 1, 0);
    lv_obj_set_style_radius(_mapBtn, 3, 0);
    lv_obj_set_style_pad_all(_mapBtn, 0, 0);
    lv_obj_set_style_shadow_width(_mapBtn, 0, 0);
    lv_obj_t* mLbl = lv_label_create(_mapBtn);
    lv_obj_set_style_text_font(mLbl, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(mLbl, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_text(mLbl, "OPEN MAP");
    lv_obj_center(mLbl);
    lv_obj_add_event_cb(_mapBtn, [](lv_event_t* e) {
        auto* self = (LvGpsScreen*)lv_event_get_user_data(e);
        if (self->_onOpenMap) self->_onOpenMap();
    }, LV_EVENT_CLICKED, this);

    _timer = nullptr;
}

void LvGpsScreen::destroyUI() {
    if (_timer) {
        lv_timer_del(_timer);
        _timer = nullptr;
    }
    _backBtn = nullptr;
    _title = nullptr;
    _body = nullptr;
    _refreshBtn = nullptr;
    _mapBtn = nullptr;
    LvScreen::destroyUI();
}

void LvGpsScreen::onEnter() {
    rebuild();
    if (!_timer) {
        _timer = lv_timer_create([](lv_timer_t* t) {
            auto* self = (LvGpsScreen*)t->user_data;
            if (self) self->rebuild();
        }, 2000, this);
    }
}

void LvGpsScreen::onExit() {
    if (_timer) {
        lv_timer_del(_timer);
        _timer = nullptr;
    }
}

void LvGpsScreen::refreshUI() {
    rebuild();
}

void LvGpsScreen::rebuild() {
    if (!_body) return;

    char buf[640];
    if (!_gps) {
        lv_label_set_text(_body, "GPS unavailable");
        return;
    }

    const bool running = _gps->isRunning();
    const bool fix = _gps->hasLocationFix();
    const int sats = _gps->satellites();
    const double alt = _gps->altitude();
    const double lat = _gps->latitude();
    const double lon = _gps->longitude();

    const char* fixStr;
    if (!running) {
        fixStr = "OFF";
    } else if (fix) {
        fixStr = "FIX";
    } else {
        fixStr = "SEARCH";
    }

    char status[80];
    if (fix) {
        snprintf(status, sizeof(status), "%s  %d sats  %.0f m", fixStr, sats, alt);
    } else {
        snprintf(status, sizeof(status), "%s  %d sats", fixStr, sats);
    }

    if (!fix) {
        snprintf(buf, sizeof(buf),
                 "%s\n\nNo fix — waiting for satellites",
                 status);
        lv_label_set_text(_body, buf);
        return;
    }

    char dd[72], dm[80], dms[80], plus[24], utm[48];
    CoordFormat::formatDD(dd, sizeof(dd), lat, lon);
    CoordFormat::formatDM(dm, sizeof(dm), lat, lon);
    CoordFormat::formatDMS(dms, sizeof(dms), lat, lon);
    CoordFormat::formatPlusCode(plus, sizeof(plus), lat, lon);
    CoordFormat::formatUTM(utm, sizeof(utm), lat, lon);

    snprintf(buf, sizeof(buf),
             "%s\n\n"
             "DD\n%s\n\n"
             "DM\n%s\n\n"
             "DMS\n%s\n\n"
             "PLUS  %s\n"
             "UTM   %s",
             status, dd, dm, dms, plus, utm);

    lv_label_set_text(_body, buf);
}

bool LvGpsScreen::handleKey(const KeyEvent& event) {
    if (event.character == 0x1B || event.del || event.character == 0x08) {
        if (_onBack) {
            _onBack();
            return true;
        }
    }
    return false;
}
