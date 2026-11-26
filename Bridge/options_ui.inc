static constexpr BridgeRect gear_tile = {809, 6, 84, 84};
static constexpr BridgeRect speaker_tile = {800, 502, 91, 89};
static constexpr BridgeRect people_tile = {135, 14, 143, 57};
static constexpr int scroll_top = 35, scroll_left = 220, scroll_width = 584, scroll_roller_h = 24;
static constexpr BridgeRect options_close = {723, 74, 49, 48};
static constexpr int options_bar_x = 441, options_slider_width = 255;
static constexpr int options_toggle_x = 724, options_check_width = 47;
static constexpr int options_slider_touch_radius = 25, options_check_touch_radius = 25;

static int scroll_roller_offset(int height) {
    return std::max(0, height - scroll_roller_h);
}

struct OptionsLayout {
    bool timer, fifo;

    int checkbox_y(int row) const { return row == 2 ? 300 : 366 + (timer ? 58 : 0); }
    int slider_y(int row) const {
        return row < 2 ? 167 + row * 66 : checkbox_y(row) + 52;
    }
    bool has_slider(int row) const { return row < 2 || (row == 2 ? timer : fifo); }
    int footer_y() const { return (fifo ? slider_y(3) : checkbox_y(3)) + 36; }
    int open_height() const { return footer_y() + 31 + scroll_roller_h - scroll_top; }
    int reveal_height(unsigned elapsed_us, unsigned duration_us) const {
        constexpr unsigned scale = 4096;
        constexpr uint64_t full = uint64_t(scale) * scale * scale;
        const unsigned t = elapsed_us * scale / duration_us;
        const uint64_t eased = uint64_t(t) * t * (3 * scale - 2 * t);
        return (open_height() * eased + full / 2) / full;
    }
};

static bool options_contains(BridgeRect r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

struct OptionsView {
    unsigned sfx, music, delay, count, capacity;
    bool muted;
};

class OptionsPainter {
public:
    uint16_t* pixels;
    int width;
    BridgeRect viewport;
    int top, bottom;

    BridgeRect screen_rect(BridgeRect r) const {
        if (viewport.w == BRIDGE_PANEL_WIDTH && viewport.h == BRIDGE_PANEL_HEIGHT)
            return {viewport.x + r.x, viewport.y + r.y, r.w, r.h};
        const int x = r.x * viewport.w / BRIDGE_PANEL_WIDTH;
        const int y = r.y * viewport.h / BRIDGE_PANEL_HEIGHT;
        return {viewport.x + x, viewport.y + y,
                (r.x + r.w) * viewport.w / BRIDGE_PANEL_WIDTH - x,
                (r.y + r.h) * viewport.h / BRIDGE_PANEL_HEIGHT - y};
    }

    bool visible(int y, int height) const {
        const auto r = screen_rect({0,y,0,height});
        return r.y < bottom && r.y + r.h > top;
    }

    void fill(BridgeRect art, uint16_t color) const {
        if (art.w <= 0 || art.h <= 0) return;
        const auto r = screen_rect(art);
        const int y0 = r.y < top ? top : r.y;
        const int y1 = r.y + r.h > bottom ? bottom : r.y + r.h;
        for (int y = y0; y < y1; ++y) {
            uint16_t* out = pixels + size_t(y) * width + r.x;
            std::fill(out, out + r.w, color);
        }
    }
};

static constexpr uint16_t ui_outline = color565(0x171b2b);
static constexpr uint16_t ui_shadow = color565(0x34304a);
static constexpr uint16_t ui_text = color565(0xffecc6);
static constexpr uint16_t ui_amber = color565(0xf8bf6a);
static constexpr uint16_t ui_cyan = color565(0x9ce4db);

static void options_plate(const OptionsPainter& p, BridgeRect r, uint16_t face, uint16_t edge) {
    const int x = r.x, y = r.y, w = r.w, h = r.h;
    p.fill({x + 4,y,w - 8,h}, ui_outline);
    p.fill({x,y + 4,w,h - 10}, ui_outline);
    p.fill({x + 4,y + 8,w - 8,h - 12}, ui_shadow);
    p.fill({x + 4,y + 4,w - 8,h - 14}, face);
    p.fill({x + 8,y + 4,w - 16,3}, edge);
    p.fill({x + 7,y + 9,3,h - 22}, edge);
    p.fill({x + 10,y + h - 13,w - 20,3}, ui_outline);
}

static void options_text(const OptionsPainter& p, int x, int y, int unit,
                         const char* label, uint16_t color, bool signature = false) {
    // Five-by-seven pixel lettering with chamfered corners and heavier capitals.
    static constexpr char characters[] = "0123456789ACEFILMNOPRSTUVX/-D.GKJ()aehimoprsztu:";
    static constexpr uint8_t glyphs[][7] = {
        {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
        {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
        {6,8,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
        {14,17,17,15,1,2,12},
        {14,27,27,31,27,27,27},{14,27,24,24,24,27,14},{31,24,24,30,24,24,31},
        {31,24,24,30,24,24,24},{14,4,4,4,4,4,14},{24,24,24,24,24,24,31},
        {17,27,31,21,17,17,17},{17,25,25,21,19,19,17},{14,27,27,27,27,27,14},
        {30,27,27,30,24,24,24},{30,27,27,30,28,26,27},{15,24,24,14,3,3,30},
        {31,31,4,4,4,4,4},{27,27,27,27,27,27,14},{27,27,27,27,27,14,4},
        {17,27,10,4,10,27,17},{1,1,2,4,8,16,16},{0,0,0,31,0,0,0},
        {30,27,27,27,27,27,30},{0,0,0,0,0,6,6},
        {14,27,24,27,27,27,15},{27,26,28,24,28,26,27},
        {7,2,2,2,2,18,12},{2,4,8,8,8,4,2},{8,4,2,2,2,4,8},
        {0,0,14,1,15,17,15},{0,0,14,17,31,16,14},{16,16,22,25,17,17,17},
        {4,0,12,4,4,4,14},{0,0,26,21,21,21,21},{0,0,14,17,17,17,14},
        {0,0,30,17,30,16,16},{0,0,22,25,16,16,16},{0,0,15,16,14,1,30},
        {0,0,31,2,4,8,31},
        {8,8,30,8,8,9,6},{0,0,17,17,17,19,13},{0,4,4,0,4,4,0},
    };
    static_assert(sizeof(characters) - 1 == sizeof(glyphs) / sizeof(glyphs[0]), "Font map size");
    if (!p.visible(y, unit * 7)) return;
    const char* first = label;
    for (; *label; ++label, x += unit * 6) {
        const char* glyph = strchr(characters, *label);
        if (!glyph) continue;
        const auto& mask = glyphs[glyph - characters];
        const int weight = !signature ? 0
            : *label == 'J' ? 2
            : label - first < 5 || *label == 'R' ? 1 : 0;
        for (int row = 0; row < 7; ++row) {
            const int slant = signature ? (6 - row) * unit / 3 : 0;
            for (int col = 0; col < 5; ++col)
                if (mask[row] & (16 >> col))
                    p.fill({x + col * unit + slant,y + row * unit,unit + weight,unit}, color);
        }
    }
}

static void options_number(const OptionsPainter& p, int center, int y, unsigned value,
                           uint16_t color, bool seconds = false) {
    char label[4] = {};
    unsigned length = 0;
    if (value >= 10) label[length++] = '1';
    label[length++] = char('0' + value % 10);
    const int start = center - (int(length) * 18 - 3 + (seconds ? 12 : 0)) / 2;
    options_text(p, start, y, 3, label, color);
    if (seconds) options_text(p, start + int(length) * 18, y + 7, 2, "S", color);
}

#include "settings_gear_sprite.inc"

static constexpr int settings_gear_cosines[] = {
    1024,1024,1023,1021,1018,1015,1011,1007,1002,996,989,982,974,965,956,946,935
};
static constexpr int settings_gear_sines[] = {
    0,27,54,80,107,134,160,187,213,239,265,291,316,342,367,392,416
};

static unsigned settings_gear_pose(unsigned motion, unsigned duration) {
    constexpr unsigned last_pose = sizeof(settings_gear_sines) / sizeof(settings_gear_sines[0]) - 1;
    return (motion * last_pose + duration / 2) / duration;
}

static void draw_gear_tile(const OptionsPainter& p, unsigned pose) {
    constexpr int size = 80, canvas = 50;
    constexpr int width = sizeof(settings_gear_pixels[0]) - 1;
    constexpr int height = sizeof(settings_gear_pixels) / sizeof(settings_gear_pixels[0]);
    // Center of the hub relative to the artwork and the padded canvas.
    constexpr int hub_x = 20, hub_y = 21, inset_x = 5, inset_y = 2;
    const int cosine = settings_gear_cosines[pose], sine = settings_gear_sines[pose];
    constexpr int denominator = 1024 * 2 * size;
    const int left = gear_tile.x + (gear_tile.w - size) / 2;
    const int top = gear_tile.y + (gear_tile.h - size) / 2;
    for (int row = 0; row < size; ++row) {
        uint8_t indices[size];
        const int y = (2 * row + 1) * canvas - 2 * size * (hub_y + inset_y);
        for (int col = 0; col < size; ++col) {
            const int x = (2 * col + 1) * canvas - 2 * size * (hub_x + inset_x);
            // Inverse rotation with nearest-neighbor sampling keeps the palette intact.
            const int u = hub_x * denominator + x * cosine + y * sine;
            const int v = hub_y * denominator + y * cosine - x * sine;
            indices[col] = u >= 0 && u < width * denominator && v >= 0 && v < height * denominator
                ? settings_gear_pixels[v / denominator][u / denominator] - '0' : 0;
        }
        for (int first = 0; first < size;) {
            int end = first + 1;
            while (end < size && indices[end] == indices[first]) ++end;
            if (indices[first]) {
                p.fill({left + first, top + row, end - first, 1}, settings_gear_palette[indices[first]]);
            }
            first = end;
        }
    }
}

static void draw_speaker_tile(const OptionsPainter& p, bool muted) {
    const auto r = speaker_tile;
    constexpr int pixel = 4;
    const int x = r.x+3, y = r.y+10;
    const auto dot = [&](int col, int row, uint16_t color) {
        p.fill({x+col*pixel,y+row*pixel,pixel,pixel}, color);
    };
    constexpr int cone_left[] = {10,9,8,7,6,5,1,0,0,0,0,1,5,6,7,8,9,10,11};
    const uint16_t face = color565(muted ? 0xb7c7c5 : 0xffdda6);
    const uint16_t light = color565(muted ? 0xe6f2eb : 0xfff3cf);
    const uint16_t shade = color565(muted ? 0x778e98 : 0xc0845e);
    const uint16_t edge = color565(muted ? 0x3c5963 : 0x764b36);
    for (int row = 0; row < 19; ++row) {
        const int right = row == 0 || row == 18 ? 11 : 12;
        for (int col = cone_left[row]; col <= right; ++col) {
            uint16_t color = edge;
            if (row > 0 && row < 18 && col > cone_left[row] && col < right &&
                col >= cone_left[row-1] && col >= cone_left[row+1]) {
                if (col < 5) color = (row < 9) ? ui_cyan : color565(0x41878b);
                else if (col == 11 || row >= 12) color = shade;
                else color = row < 7 ? light : face;
            }
            dot(col,row,color);
        }
    }
    if (muted) {
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 7; ++col) {
                const int distance = std::min(std::abs(row-col),std::abs(row+col-6));
                if (distance <= 1)
                    dot(14+col,6+row,distance ? color565(0xa94d50) : color565(0xff897f));
            }
        }
    } else {
        constexpr int inner[] = {0,3,5,6,5,3,0};
        constexpr int outer[] = {0,3,6,8,9,10,10,10,9,8,6,3,0};
        const auto wave = [&](const int* offsets, int length, int left, int top, uint16_t highlight) {
            for (int row = 0; row < length; ++row) {
                const int px = left + offsets[row], py = top + row * pixel;
                p.fill({px,py,5,pixel},highlight);
            }
        };
        wave(inner,7,x+56,y+24,ui_cyan);
        wave(outer,13,x+70,y+12,ui_amber);
    }
}

static void draw_people_tile(const OptionsPainter& p, const OptionsView& view) {
    const auto r = people_tile;
    constexpr int unit = 3;
    const int x = r.x+28, y = r.y+27;
    for (int row = -8; row <= 8; ++row) {
        for (int col = -8; col <= 8; ++col) {
            const int radius_sq = row*row + col*col;
            if (radius_sq > 70) continue;
            const int head_y = row + 3;
            const bool head = col*col + head_y*head_y <= 7;
            const int shoulder_x = std::max(0,std::abs(col)-1);
            const int shoulder_y = std::max(0,5-row);
            const bool shoulders = row >= 2 && shoulder_x*shoulder_x + shoulder_y*shoulder_y <= 22;
            if (radius_sq < 50 && !head && !shoulders) continue;
            const uint16_t color = row < 2 ? color565(0xd9f9f2) : ui_cyan;
            p.fill({x+col*unit,y+row*unit,unit,unit},color);
        }
    }
    const int number_x = r.x+65, number_y = r.y+14;
    const char label[] = {char('0'+view.count), '/', char('0'+view.capacity), '\0'};
    options_text(p,number_x+2,number_y+2,4,label,color565(0x204643));
    options_text(p,number_x,number_y,4,label,view.count == view.capacity ? ui_amber : ui_text);
}

static constexpr uint16_t parchment_ink = color565(0x4e2e24);
static constexpr uint16_t parchment_fold = color565(0xd8a87c);
static constexpr uint16_t parchment = color565(0xffe7bd);
static constexpr uint16_t parchment_light = color565(0xfff1d1);
static constexpr uint16_t parchment_inset = color565(0xf6d6a9);
static constexpr uint16_t brass = color565(0xf2b869);
static constexpr uint16_t settings_walnut = color565(0x45251f);
static constexpr uint16_t settings_wood = color565(0x883e2b);
static constexpr uint16_t settings_copper = color565(0xc16b42);
static constexpr uint16_t settings_teal = color565(0x42dac0);

static void draw_option_check(const OptionsPainter& p, int y, bool enabled) {
    const int x = options_toggle_x, w = options_check_width;
    p.fill({x+4,y-22,w-8,44}, settings_walnut);
    p.fill({x,y-18,w,36}, settings_walnut);
    p.fill({x+4,y-18,w-8,36}, settings_copper);
    p.fill({x+7,y-18,w-14,3}, brass);
    p.fill({x+8,y-13,w-16,26}, enabled ? color565(0x13796f) : color565(0x683e34));
    p.fill({x+8,y-13,w-16,3}, enabled ? settings_teal : color565(0x382120));
    if (enabled) {
        p.fill({x+10,y-2,7,6}, parchment_light);
        p.fill({x+15,y+3,8,6}, parchment_light);
        p.fill({x+21,y-2,7,6}, parchment_light);
        p.fill({x+26,y-7,7,6}, parchment_light);
        p.fill({x+31,y-12,5,6}, parchment_light);
    }
}

static void draw_settings_icon(const OptionsPainter& p, int x, int y, int icon) {
    // Speaker, note, hourglass and group of people.
    static constexpr char icons[4][11][14] = {
        {".....1.....","....12.....","...122..1..",".11222...1.","122222.1..1","122222..1.1",
         "122222.1..1",".11222...1.","...122..1..","....12.....",".....1....."},
        {".....11111.",".....12221.",".....11121.",".....1..21.",".....1..21.",".....1..21.",
         ".....1.121.","...111.221.","..1221.11..","..1221.....","...11......"},
        {".111111111.",".122222221.","..1.....1..","..1222221..","...12221...","....121....",
         "...12221...","..1222221..","..1.....1..",".122222221.",".111111111."},
        {".....111.....","....12221....","....12221....",".11..111..11.","1331.....1331",
         ".11..111..11.",".11.12221.11.","1331222221331","1331222221331",".11122222111.","...1111111..."},
    };
    constexpr int unit = 3;
    const bool group = icon == 3;
    const int width = group ? 13 : 11;
    if (group) x -= unit;
    const uint16_t palette[] = {
        group ? color565(0x1b656a) : settings_walnut,
        icon == 0 ? settings_copper : settings_teal,
        color565(0x8de7d7),
    };
    for (int row = 0; row < 11; ++row)
        for (int col = 0; col < width; ++col) {
            const char px = icons[icon][row][col];
            if (px != '.') p.fill({x+col*unit,y+row*unit,unit,unit}, palette[px-'1']);
        }
}

static void draw_scroll_roller(const OptionsPainter& p, int y) {
    const int x = scroll_left, w = scroll_width;
    if (!p.visible(y, scroll_roller_h)) return;
    p.fill({x+4,y,w-8,scroll_roller_h}, settings_walnut);
    p.fill({x,y+4,w,scroll_roller_h-8}, settings_walnut);
    p.fill({x+10,y+4,w-20,scroll_roller_h-9}, settings_wood);
    p.fill({x+12,y+4,w-24,4}, settings_copper);
    p.fill({x+24,y+4,w-48,1}, brass);
    p.fill({x+22,y+16,w-44,3}, color565(0x673022));
    for (int cap : {x+4,x+w-25}) {
        p.fill({cap,y+3,21,18}, settings_walnut);
        p.fill({cap+2,y+3,17,16}, settings_copper);
        p.fill({cap+3,y+4,14,10}, brass);
        p.fill({cap+5,y+4,10,2}, parchment_light);
        p.fill({cap+7,y+8,7,8}, color565(0x236a64));
        p.fill({cap+8,y+9,4,4}, settings_teal);
    }
}

static void draw_option_slider(const OptionsPainter& p, int y, unsigned value, int divisions,
                               uint16_t accent, bool seconds = false) {
    constexpr uint16_t slot = color565(0x35201a), grain = color565(0xa4633e);
    constexpr int left = options_bar_x, right = left + options_slider_width;
    const int thumb = left + int(value) * options_slider_width / divisions;
    // Recessed slot, mechanical detents and the ribbed fader cap.
    p.fill({left-21,y-24,right-left+42,48}, settings_walnut);
    p.fill({left-23,y-20,right-left+46,40}, settings_walnut);
    p.fill({left-20,y-21,right-left+40,42}, color565(0x8c4e32));
    p.fill({left-19,y-20,right-left+38,2}, color565(0xcd8955));
    for (BridgeRect streak : {BridgeRect{-14,-17,54,1}, {54,-16,79,1},
                              {154,-17,69,1}, {235,-14,31,1},
                              {4,18,89,1}, {110,19,71,1}, {199,18,68,1}}) {
        streak.x += left;
        streak.y += y;
        p.fill(streak,grain);
    }
    p.fill({left-10,y-5,right-left+20,10}, slot);
    p.fill({left-8,y+4,right-left+16,2}, color565(0xb67547));
    for (int division = 0; division <= divisions; ++division) {
        const int x = left + division * options_slider_width / divisions;
        const bool major = division == 0 || division == divisions || 2 * division == divisions;
        p.fill({x,y-15,2,major ? 7 : 4}, color565(0xf0be83));
        p.fill({x-3,y+13,7,4}, division <= int(value) && value != 0 ? accent : color565(0x613925));
    }
    p.fill({thumb-18,y-13,37,28}, slot);
    p.fill({thumb-15,y-15,31,31}, slot);
    p.fill({thumb-15,y-12,31,24}, color565(0xc48651));
    p.fill({thumb-14,y-12,29,4}, color565(0xf8c389));
    p.fill({thumb-14,y+9,29,3}, color565(0x94522f));
    for (int groove : {-9, -5, 4, 8}) {
        p.fill({thumb+groove,y-7,2,15}, color565(0x7a4229));
        p.fill({thumb+groove+2,y-7,1,15}, color565(0xe4a96f));
    }
    p.fill({thumb-1,y-10,3,21}, accent);
    p.fill({721,y-20,54,40}, settings_walnut);
    p.fill({724,y-17,48,34}, color565(0x45271f));
    p.fill({724,y+15,48,2}, settings_copper);
    options_number(p,747,y-10,value,accent,seconds);
}

static void draw_options_panel(OptionsPainter p, const OptionsView& view,
                               const OptionsLayout& layout, int height) {
    const int bottom_roller = scroll_top + scroll_roller_offset(height);
    p.top = std::max(p.top, p.screen_rect({scroll_left,scroll_top,1,1}).y);
    if (height <= scroll_roller_h) {
        p.bottom = std::min(p.bottom, p.screen_rect({scroll_left,scroll_top + height,1,1}).y);
        draw_scroll_roller(p,scroll_top);
        return;
    }
    constexpr int paper_x = scroll_left + 8, paper_w = scroll_width - 16;
    constexpr int paper_y = scroll_top + 14;
    const int paper_h = bottom_roller + 10 - paper_y;
    // Paint the broad paper surface once per refresh. The rails use narrow strips.
    p.fill({paper_x+18,paper_y,paper_w-33,paper_h},parchment);
    p.fill({paper_x+15,paper_y,3,paper_h},parchment_light);
    for (int x : {paper_x,paper_x+paper_w-13}) {
        const bool right = x != paper_x;
        p.fill({x+(right ? 9 : 0),paper_y,4,paper_h},settings_walnut);
        p.fill({x+(right ? 0 : 4),paper_y,9,paper_h},settings_wood);
        p.fill({x+(right ? 3 : 5),paper_y,3,paper_h},settings_copper);
        p.fill({x+(right ? -2 : 13),paper_y,2,paper_h},parchment_fold);
    }

    OptionsPainter text = p;
    text.bottom = std::min(text.bottom, text.screen_rect({paper_x,bottom_roller,1,1}).y);
    constexpr int rail_x = paper_x + 20, rail_width = paper_w - 40;
    constexpr int label_x = 296;
    if (text.visible(67,62)) {
        text.fill({rail_x+4,67,rail_width-8,60},settings_walnut);
        text.fill({rail_x,72,rail_width,50},settings_walnut);
        text.fill({rail_x+4,72,rail_width-8,50},color565(0x602e26));
        text.fill({rail_x+7,72,rail_width-14,2},settings_copper);
        text.fill({rail_x+17,118,208,2},settings_teal);
        text.fill({rail_x+231,116,5,5},brass);
        options_text(text,rail_x+17,80,5,"SETTINGS",color565(0xa66547));
        options_text(text,rail_x+16,78,5,"SETTINGS",parchment_light);
        options_plate(text,options_close,color565(0x9f4e33),brass);
        options_text(text,options_close.x+15,options_close.y+8,4,"X",parchment_light);
    }

    const auto ruled_row = [&](int top, int bottom) {
        text.fill({rail_x+4,top,rail_width-8,bottom-top},parchment_fold);
        text.fill({rail_x,top+4,rail_width,bottom-top-8},parchment_fold);
        text.fill({rail_x+2,top+4,rail_width-4,bottom-top-8},parchment_inset);
        text.fill({rail_x+5,top+2,rail_width-10,2},parchment_light);
        text.fill({rail_x+6,bottom-5,rail_width-12,2},color565(0xe8bd8f));
    };
    constexpr const char* audio_labels[] = {"VFX","MUSIC"};
    const unsigned audio_values[] = {view.sfx,view.music};
    const uint16_t audio_colors[] = {color565(0xffbd69), settings_teal};
    for (int row = 0; row < 2; ++row) {
        const int y = layout.slider_y(row);
        if (!text.visible(y-28,56)) continue;
        ruled_row(y-28,y+28);
        draw_settings_icon(text,rail_x+10,y-16,row);
        options_text(text,label_x,y-14,4,audio_labels[row],settings_walnut);
        draw_option_slider(text,y,audio_values[row],10,audio_colors[row]);
    }

    constexpr const char* labels[] = {"TIMER.DOOR", "STACK.MODE"};
    const unsigned values[] = {view.delay,view.capacity};
    const uint16_t colors[] = {color565(0xadd783), color565(0xc6aaff)};
    for (int row = 2; row < 4; ++row) {
        const int y = layout.checkbox_y(row);
        const int slider_y = layout.slider_y(row);
        const bool enabled = layout.has_slider(row);
        const int row_end = enabled ? slider_y + 28 : y + 28;
        if (!text.visible(y-28,row_end-y+28)) continue;
        ruled_row(y-28,row_end);
        if (text.visible(y-22,44)) {
            draw_settings_icon(text,rail_x+10,y-16,row);
            options_text(text,label_x,y-14,4,labels[row-2],settings_walnut);
            draw_option_check(text,y,enabled);
        }
        if (enabled && text.visible(slider_y-24,48))
            draw_option_slider(text,slider_y,values[row-2],row == 2 ? 10 : 5,colors[row-2],row == 2);
    }
    const int footer = layout.footer_y();
    if (text.visible(footer,31)) {
        text.fill({rail_x+5,footer+1,rail_width-10,2},parchment_fold);
        text.fill({rail_x+5,footer+3,rail_width-10,2},parchment_light);
        constexpr char author[] = "Autor: Joseph Ramirez (2021-0538)";
        constexpr int signature_width = (sizeof(author) - 1) * 12 + 2;
        const int right = rail_x + rail_width - 11;
        text.fill({right-signature_width-8,footer+7,signature_width+16,21},parchment_inset);
        options_text(text,right-signature_width,footer+10,2,author,settings_walnut,true);
    }

    p.fill({paper_x+15,bottom_roller-4,paper_w-30,5},parchment_fold);
    draw_scroll_roller(p,bottom_roller);
    // Keep the upper roller in front while the lower one slides out beneath it.
    draw_scroll_roller(p,scroll_top);
}
