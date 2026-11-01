// Compact Raspberry Pi 5 agent console: navigation rail, health, proxy, traffic.
#include "rp5_view.h"
#include "rp5_monitor.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

LV_FONT_DECLARE(ui_font_lato_12);
LV_FONT_DECLARE(ui_font_lato_20);
#define META (&ui_font_lato_12)
#define TITLE (&ui_font_lato_20)
#define BG 0x111315
#define PANEL 0x191C1F
#define HEADER 0x25282C
#define BORDER 0x30343A
// Grid lines carry the axis reading, so they are not decoration: #30343A measured
// 1.37:1 against the panel, far under the 3:1 floor for essential graphics.
#define GRID 0x454A52
#define TEXT 0xEBEDF0
#define MUTED 0x9299A3
#define BLUE 0x579AFF
// TX was #AC8CEB: relative luminance 0.335 against RX's 0.324, i.e. the two series
// differed only in hue and collapsed into one line at low backlight or in grayscale.
#define PURPLE 0xC9A6FF
#define GREEN 0x73B880
#define AMBER 0xD9B56D
#define RED 0xE67E80
#define NAV_ON 0x22406B
#define NAV_ON_ICON 0x8FC0FF
#define VIEWS 3
#define WINDOW_SEC 60
#define BAR_FULL_PX 126
// Chart axis: four equal rows so every horizontal grid line lands on a 1-2-5 step
// (§5.3) instead of an arbitrary split of a rounded range, and three of them so the
// 34 px plot does not turn into 6.8 px stripes.
#define SCALE_MIN_STEP 250          /* kbps -> 1 Mbps axis floor, was implicit in peak init */
#define SCALE_ROWS 4
#define SCALE_STEPS 4
_Static_assert(RP5_CURVE_MAX > WINDOW_SEC, "1 Hz history must cover the full chart window");

static const char *titles[VIEWS] = {"Health","Proxy","Gateway"};
static lv_obj_t *pages[VIEWS],*nav[VIEWS],*nav_icon[VIEWS],*title,*footer,*status;
static lv_obj_t *temp,*load,*cpu,*memory,*uptime,*disk,*freq,*agent,*collector;
static lv_obj_t *bars[2];
static lv_obj_t *px_node,*px_delay,*px_conns,*px_down,*px_up,*px_mode,*px_in,*px_out;
static lv_obj_t *rates[2],*traffic_scale,*lan_line;
static int selected;
static uint32_t last_chart_ts;
static bool last_chart_valid;
static int32_t chart_plot_w;
static int32_t last_scroll,last_scale_drawn;
static rp5_curve_t animation_curve;
static int32_t animation_scale=SCALE_MIN_STEP*SCALE_ROWS,scale_pending;
static int32_t scale_from,scale_to;
static int scale_step_index=SCALE_STEPS;      /* SCALE_STEPS = staircase settled */
static uint64_t scale_next_ms;
static uint64_t motion_last_ms;
typedef struct {lv_obj_t *obj,*empty;lv_chart_series_t *rx,*tx;} activity_t;
static activity_t traffic;

static lv_obj_t *box(lv_obj_t *parent,int x,int y,int w,int h,uint32_t color,int radius){
    lv_obj_t *o=lv_obj_create(parent);lv_obj_remove_style_all(o);
    lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(o,lv_color_hex(color),0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
    lv_obj_set_style_radius(o,radius,0);return o;
}
static lv_obj_t *label(lv_obj_t *p,int x,int y,int w,const lv_font_t *font,uint32_t color,const char *text){
    lv_obj_t *o=lv_label_create(p);lv_obj_set_pos(o,x,y);lv_obj_set_width(o,w);
    lv_obj_set_style_text_font(o,font,0);lv_obj_set_style_text_color(o,lv_color_hex(color),0);
    lv_label_set_long_mode(o,LV_LABEL_LONG_DOT);lv_label_set_text(o,text);return o;
}
static void text(lv_obj_t *o,const char *s){if(strcmp(lv_label_get_text(o),s))lv_label_set_text(o,s);}
static void tint(lv_obj_t *o,uint32_t c){if(!lv_color_eq(lv_obj_get_style_text_color(o,0),lv_color_hex(c)))lv_obj_set_style_text_color(o,lv_color_hex(c),0);}
static void fill(lv_obj_t *o,uint32_t c){if(!lv_color_eq(lv_obj_get_style_bg_color(o,0),lv_color_hex(c)))lv_obj_set_style_bg_color(o,lv_color_hex(c),0);}
static void align_right(lv_obj_t *o){lv_obj_set_style_text_align(o,LV_TEXT_ALIGN_RIGHT,0);}
static void glyph(lv_obj_t *p,int kind,uint32_t color){
    if(kind==0){for(int i=0;i<4;i++)box(p,4+(i%2)*7,5+(i/2)*7,5,5,color,1);}       // health: 2x2 tiles
    else if(kind==1){lv_obj_t *o=box(p,4,4,13,13,PANEL,7);lv_obj_set_style_border_width(o,1,0);lv_obj_set_style_border_color(o,lv_color_hex(color),0);box(p,9,9,3,3,color,2);} // proxy: node dot
    else {for(int i=0;i<3;i++)box(p,3+i*5,14-i*4,3,4+i*4,color,1);}                // gateway: traffic bars
}
// Chart axis: four equal rows so every horizontal grid line lands on a 1-2-5 step
// (§5.3) instead of an arbitrary split of a rounded range. See SCALE_* defines.
static void chart_build(lv_obj_t *p,int x,int y,int w,int h){
    activity_t *c=&traffic;
    c->obj=lv_chart_create(p);lv_obj_set_pos(c->obj,x,y);lv_obj_set_size(c->obj,w,h);
    lv_obj_set_style_bg_color(c->obj,lv_color_hex(PANEL),0);lv_obj_set_style_bg_opa(c->obj,LV_OPA_COVER,0);
    lv_obj_set_style_border_width(c->obj,0,0);lv_obj_set_style_pad_all(c->obj,0,0);lv_obj_set_style_radius(c->obj,0,0);
    lv_obj_set_style_line_color(c->obj,lv_color_hex(GRID),LV_PART_MAIN);lv_obj_set_style_line_width(c->obj,1,LV_PART_MAIN);
    lv_obj_set_style_line_width(c->obj,2,LV_PART_ITEMS);lv_obj_set_style_size(c->obj,0,0,LV_PART_INDICATOR);
    lv_chart_set_type(c->obj,LV_CHART_TYPE_SCATTER);lv_chart_set_point_count(c->obj,RP5_CURVE_MAX);
    lv_chart_set_div_line_count(c->obj,SCALE_ROWS-1,5);
    lv_chart_set_range(c->obj,LV_CHART_AXIS_PRIMARY_X,0,WINDOW_SEC*1000);lv_chart_set_range(c->obj,LV_CHART_AXIS_PRIMARY_Y,0,SCALE_MIN_STEP*SCALE_ROWS);
    c->rx=lv_chart_add_series(c->obj,lv_color_hex(BLUE),LV_CHART_AXIS_PRIMARY_Y);
    c->tx=lv_chart_add_series(c->obj,lv_color_hex(PURPLE),LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(c->obj,c->rx,LV_CHART_POINT_NONE);lv_chart_set_all_value(c->obj,c->tx,LV_CHART_POINT_NONE);
    c->empty=label(p,x,y+h/2-7,w,META,MUTED,"Waiting for data");lv_obj_set_style_text_align(c->empty,LV_TEXT_ALIGN_CENTER,0);
}
// ─── motion ────────────────────────────────────────────────────────
// The agent samples at 1 Hz and the UI polls at 10 Hz, so a fixed-duration
// lv_anim is the wrong model: a 320 ms ease gets deleted and restarted long
// before it elapses, and the 420 ms bar on the same sample is a second curve
// with its own phase (§6.7: one quantity must have one animation). Every
// displayed quantity is instead one Q16 exponential approach (§3.2) stepped
// from the frame timer with a dt-aware alpha, so the settle time does not
// depend on frame rate and text, bar and semantic color read one value.
#define TAU_MS 150                 // §3.2: 1% in 691 ms, inside the 1 Hz sample period
// Longest frame gap the table covers. Measured on hardware: this panel renders at
// ~14 fps (71 ms) while animating, not the 16 ms LVGL asks for, because the CPU is
// at 160 MHz and the software renderer is the limit. Clamping below the real gap
// would silently slow every animation and break the frame-rate independence above,
// so the table has to reach past the worst observed frame. A longer stall still
// clamps low: it slows down, but α never leaves [0, 0.99] and never overshoots.
#define DT_MAX_MS 128
#define SNAP_Q16 (1 << 15)         // §3.4: under half a displayed LSB, snap rather than crawl
#define DELAY_MAX_MS 9999
// 1 ms resolution, so convergence is the same at any frame rate: a coarser bucket
// rounds dt and the error compounds once per frame.
static uint32_t alpha_q16[DT_MAX_MS + 1];

static void motion_clock_init(void) { // float is allowed here: it runs once at startup
    for (int i = 0; i <= DT_MAX_MS; i++) {
        double a = 1.0 - exp(-(double)i / (double)TAU_MS);
        if (a > 0.99) a = 0.99;
        alpha_q16[i] = (uint32_t)(a * 65536.0 + 0.5);
    }
}

typedef struct {
    lv_obj_t *label, *bar;             // bar is optional and reads the same accumulator
    int32_t value, target;             // Q16, in units of one displayed LSB
    int32_t shown, shown_bar;          // last quantizations actually painted
    int32_t deadband, heat_warn, heat_danger; // display LSB units; deadband is §3.4 hysteresis
    uint8_t scale;                     // 1 = integer, 10 = one decimal
    bool live;
    const char *unit, *dash;
} chan_t;
static chan_t ch_temp, ch_cpu, ch_mem, ch_delay;

static uint32_t heat_i(int32_t v, int32_t warn, int32_t danger) {
    return v < warn ? GREEN : v < danger ? AMBER : RED;
}
#define SHOWN_NONE INT32_MIN       // no quantization has been painted yet
static void chan_draw(chan_t *c) {
    if (!c->label) return;
    if (!c->live) {                    // no data is a state, not a value to roll through
        text(c->label, c->dash);
        tint(c->label, MUTED);
        c->shown = SHOWN_NONE;         // else the next reading looks "unchanged" and is skipped
        if (c->bar) { fill(c->bar, BORDER); if (c->shown_bar != 0) { c->shown_bar = 0; lv_obj_set_width(c->bar, 0); } }
        return;
    }
    int32_t v = (c->value + SNAP_Q16) >> 16;
    if (v != c->shown) {
        c->shown = v;
        char b[24];
        if (c->scale == 10) snprintf(b, sizeof(b), "%d.%d%s", (int)(v / 10), (int)((v < 0 ? -v : v) % 10), c->unit);
        else snprintf(b, sizeof(b), "%d%s", (int)v, c->unit);
        text(c->label, b);
    }
    uint32_t heat = heat_i(v, c->heat_warn, c->heat_danger);
    tint(c->label, heat);
    if (c->bar) {
        fill(c->bar, heat);
        int32_t w = (int32_t)(((int64_t)c->value * BAR_FULL_PX) >> 16) / 100;
        if (w != c->shown_bar) { c->shown_bar = w; lv_obj_set_width(c->bar, w); }
    }
}
static void chan_sample(chan_t *c, bool valid, int32_t v) {
    if (!valid) { c->live = false; chan_draw(c); return; }
    int32_t q = v << 16;
    if (!c->live) { c->live = true; c->value = c->target = q; }   // a dash never rolls into a number
    else if (q != c->target) {
        int32_t gap = q > c->target ? q - c->target : c->target - q;
        if (gap >= (c->deadband << 16)) c->target = q;
    }
    chan_draw(c);
}
static void motion_step(uint64_t now_ms) {
    uint64_t dt = now_ms > motion_last_ms ? now_ms - motion_last_ms : 0;
    motion_last_ms = now_ms;
    if (dt == 0) return;
    uint32_t alpha = alpha_q16[dt > DT_MAX_MS ? DT_MAX_MS : (uint32_t)dt];
    chan_t *all[] = {&ch_temp, &ch_cpu, &ch_mem, &ch_delay};
    for (unsigned i = 0; i < 4; i++) {
        chan_t *c = all[i];
        if (!c->live) continue;
        int32_t gap = c->target - c->value;
        if (gap <= SNAP_Q16 && gap >= -SNAP_Q16) { if (c->value != c->target) { c->value = c->target; chan_draw(c); } continue; }
        c->value += (int32_t)(((int64_t)gap * alpha) >> 16);
        chan_draw(c);
    }
}

static int32_t page_dir_off[VIEWS];
static void page_exec(void *var, int32_t v) {   // v is LVGL's eased 0..1024 (§7.1: 10-bit, not 16)
    lv_obj_t *page = (lv_obj_t *)var;
    int32_t from = 0;
    for (int i = 0; i < VIEWS; i++) if (pages[i] == page) { from = page_dir_off[i]; break; }
    lv_obj_set_style_translate_x(page, from * (1024 - v) / 1024, 0);
    lv_obj_set_style_opa(page, (lv_opa_t)(v * 255 / 1024), 0);
}
#define PAGE_SLIDE 34
#define PAGE_TIME 240
// One animation drives both the slide and the fade so they cannot drift apart, and
// ease_in_out zeroes the velocity at both ends, which a whole-panel move needs (§0).
static void page_in(lv_obj_t *page, int direction) {
    lv_anim_del(page, page_exec);
    int32_t from = direction < 0 ? -PAGE_SLIDE : PAGE_SLIDE;
    for (int i = 0; i < VIEWS; i++) if (pages[i] == page) page_dir_off[i] = from;
    lv_obj_set_style_translate_x(page, from, 0);
    lv_obj_set_style_opa(page, LV_OPA_TRANSP, 0);
    lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, page);
    lv_anim_set_values(&a, 0, 1024); lv_anim_set_time(&a, PAGE_TIME);
    lv_anim_set_exec_cb(&a, page_exec); lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void build_health(lv_obj_t *p){
    temp=label(p,2,0,118,TITLE,TEXT,"--.- C");
    load=label(p,124,4,148,META,MUTED,"load -- / -- / --");align_right(load);
    label(p,2,28,32,META,MUTED,"CPU");cpu=label(p,34,28,60,META,TEXT,"--%");
    label(p,143,28,34,META,MUTED,"MEM");memory=label(p,177,28,60,META,TEXT,"--%");
    box(p,2,48,126,4,BORDER,2);box(p,143,48,126,4,BORDER,2);
    bars[0]=box(p,2,48,0,4,BLUE,2);bars[1]=box(p,143,48,0,4,BLUE,2);
    box(p,0,60,274,1,BORDER,0);
    label(p,2,66,34,META,MUTED,"Up");uptime=label(p,38,66,100,META,TEXT,"--");
    label(p,143,66,32,META,MUTED,"Disk");disk=label(p,177,66,90,META,TEXT,"--");
    label(p,2,86,34,META,MUTED,"Freq");freq=label(p,48,86,80,META,TEXT,"-- GHz");
    label(p,143,86,32,META,MUTED,"Agent");agent=label(p,177,86,90,META,TEXT,"--");
    collector=label(p,2,106,270,META,MUTED,"Collector --");
    // deadband is §3.4 hysteresis on the sample, in display LSB; thresholds are the
    // same numbers the old per-field tint used, now derived from what is drawn.
    // shown starts invalid because the labels are built showing the dash.
    ch_temp=(chan_t){.label=temp,.deadband=5,.heat_warn=700,.heat_danger=800,.scale=10,.unit=" C",.dash="--.- C",.shown=SHOWN_NONE};
    ch_cpu=(chan_t){.label=cpu,.bar=bars[0],.deadband=2,.heat_warn=70,.heat_danger=90,.scale=1,.unit="%",.dash="--%",.shown=SHOWN_NONE};
    ch_mem=(chan_t){.label=memory,.bar=bars[1],.deadband=2,.heat_warn=75,.heat_danger=90,.scale=1,.unit="%",.dash="--%",.shown=SHOWN_NONE};
}
static void build_proxy(lv_obj_t *p){
    px_node=label(p,2,0,270,META,TEXT,"No node");
    px_delay=label(p,2,18,134,TITLE,TEXT,"-- ms");
    ch_delay=(chan_t){.label=px_delay,.deadband=10,.heat_warn=300,.heat_danger=800,.scale=1,.unit=" ms",.dash="-- ms",.shown=SHOWN_NONE};
    px_conns=label(p,140,26,132,META,MUTED,"-- connections");align_right(px_conns);
    box(p,0,44,274,1,BORDER,0);
    label(p,2,50,40,META,MUTED,"Down");px_down=label(p,44,50,110,META,TEXT,"--");
    label(p,143,50,26,META,MUTED,"Up");px_up=label(p,174,50,100,META,TEXT,"--");
    label(p,2,70,40,META,MUTED,"Mode");px_mode=label(p,44,70,110,META,TEXT,"--");
    box(p,0,88,274,1,BORDER,0);
    label(p,2,94,44,META,MUTED,"Total");px_in=label(p,52,94,104,META,TEXT,"--");
    label(p,143,94,40,META,MUTED,"Sent");px_out=label(p,184,94,90,META,TEXT,"--");
}
static void build_gateway(lv_obj_t *p){
    label(p,2,0,130,META,BLUE,"Receive");label(p,143,0,128,META,PURPLE,"Transmit");
    rates[0]=label(p,1,14,134,TITLE,TEXT,"--");rates[1]=label(p,143,14,131,TITLE,TEXT,"--");
    label(p,2,34,120,META,MUTED,"wan0 uplink");traffic_scale=label(p,150,34,123,META,MUTED,"-- Mbps");align_right(traffic_scale);
    chart_build(p,1,48,272,34);
    label(p,2,84,98,META,MUTED,"1 min ago");lv_obj_t *o=label(p,196,84,76,META,MUTED,"Now");align_right(o);
    box(p,0,98,274,1,BORDER,0);
    lan_line=label(p,2,104,272,META,MUTED,"LAN -- / --");
}
static void select_view(int next){
    if(next<0 || next>=VIEWS)return;
    bool changed=next!=selected;
    int direction=next>=selected?1:-1;
    selected=next;
    for(int i=0;i<VIEWS;i++){
        if(i==selected)lv_obj_remove_flag(pages[i],LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(pages[i],LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(nav[i],lv_color_hex(i==selected?NAV_ON:BG),0);
        lv_obj_clean(nav_icon[i]);glyph(nav_icon[i],i,i==selected?NAV_ON_ICON:MUTED);
    }
    text(title,titles[selected]);
    if(changed)page_in(pages[selected],direction);
    else {lv_anim_del(pages[selected],page_exec);lv_obj_set_style_translate_x(pages[selected],0,0);lv_obj_set_style_opa(pages[selected],LV_OPA_COVER,0);}
    lv_obj_invalidate(lv_screen_active());
}

void rp5_view_create(void){
    motion_clock_init();
    lv_obj_t *scr=lv_screen_active();lv_obj_remove_style_all(scr);lv_obj_set_style_bg_color(scr,lv_color_hex(BG),0);lv_obj_set_style_bg_opa(scr,LV_OPA_COVER,0);lv_obj_remove_flag(scr,LV_OBJ_FLAG_SCROLLABLE);
    box(scr,0,0,320,25,HEADER,0);label(scr,8,1,22,TITLE,BLUE,"P");label(scr,39,6,61,META,BLUE,"Raspberry");label(scr,100,6,10,META,MUTED,"/");
    title=label(scr,114,6,111,META,TEXT,"Health");status=label(scr,236,6,80,META,MUTED,"Waiting");align_right(status);
    box(scr,29,25,1,145,BORDER,0);
    for(int i=0;i<VIEWS;i++){
        nav[i]=box(scr,3,30+i*27,23,24,BG,4);nav_icon[i]=box(nav[i],1,1,21,22,BG,0);lv_obj_set_style_bg_opa(nav_icon[i],LV_OPA_TRANSP,0);
        pages[i]=box(scr,38,32,274,136,BG,0);
    }
    build_health(pages[0]);build_proxy(pages[1]);build_gateway(pages[2]);
    footer=label(scr,39,154,273,META,MUTED,"Connecting to rp5...");select_view(0);
}

static const char *failure(bool wifi){
    if(!wifi)return "Waiting for Wi-Fi";
    switch(rp5_get_link_state()){
        case RP5_LINK_CONFIG_ERROR:return "Check RP5_HOST / RP5_PORT";
        case RP5_LINK_HTTP_ERROR:return "rp5-agent returned an error";
        case RP5_LINK_STALE:return "Agent data expired";
        case RP5_LINK_TIMEOUT:return "Agent request timed out";
        default:return "Waiting for agent data";
    }
}
static void format_rate(uint32_t bytes,char *out,size_t cap){
    double mbps=(double)bytes*8/1000000;
    if(mbps>=1000)snprintf(out,cap,"%.2f Gbps",mbps/1000);
    else if(mbps>=1)snprintf(out,cap,"%.1f Mbps",mbps);
    else snprintf(out,cap,"%.0f kbps",mbps*1000);
}
static void format_bytes(uint64_t bytes,char *out,size_t cap){
    double b=(double)bytes;
    if(b>=1e12)snprintf(out,cap,"%.1f TB",b/1e12);
    else if(b>=1e9)snprintf(out,cap,"%.1f GB",b/1e9);
    else if(b>=1e6)snprintf(out,cap,"%.0f MB",b/1e6);
    else snprintf(out,cap,"%.0f kB",b/1e3);
}
// The axis label is integer-formatted: the ESP32-S3 FPU is single precision, so
// every "%.1f" with a double argument goes through software float.
static void scale_label(int32_t kbps,bool live){
    char b[32];
    if(!live)strcpy(b,"-- Mbps");
    else{int32_t t10=(kbps+50)/100;snprintf(b,sizeof(b),"%d.%d Mbps",(int)(t10/10),(int)(t10%10));}
    text(traffic_scale,b);
}
// The plot is a raster: 272 px over a 60 s window moves one pixel every 220 ms, and
// the sample clock ticks once per second. Redrawing on every frame therefore paints
// the same pixels ~13 times out of 14, so the work is gated on the quantized values
// that can actually change the image (scroll, axis, newest sample).
static void render_activity(activity_t *a,const rp5_curve_t *c,uint64_t now_ms,int32_t scale,bool valid,bool force){
    uint32_t now=now_ms/1000;
    if(chart_plot_w<=0){lv_obj_update_layout(a->obj);chart_plot_w=lv_obj_get_content_width(a->obj);}
    int32_t width=chart_plot_w;
    if(width<1)return;
    // Quantize the shared scroll offset, not each point's changing age.
    // Otherwise adjacent vertices move on different frames and stretch lines.
    int32_t scroll=(int32_t)((uint64_t)now_ms*width/(WINDOW_SEC*1000));
    if(!force&&scroll==last_scroll&&scale==last_scale_drawn)return;
    last_scroll=scroll;last_scale_drawn=scale;
    lv_chart_set_range(a->obj,LV_CHART_AXIS_PRIMARY_X,0,width);
    int32_t *rx_x=lv_chart_get_x_array(a->obj,a->rx),*tx_x=lv_chart_get_x_array(a->obj,a->tx);
    int32_t *rx_y=lv_chart_get_y_array(a->obj,a->rx),*tx_y=lv_chart_get_y_array(a->obj,a->tx);
    for(int i=0;i<RP5_CURVE_MAX;i++){
        int32_t x=0,rx=LV_CHART_POINT_NONE,tx=LV_CHART_POINT_NONE;
        if(valid && i<c->n){int slot=(c->head-c->n+i+RP5_CURVE_MAX)%RP5_CURVE_MAX;
            uint32_t stamp=c->sample_ts[slot];
            bool gap=i && stamp-c->sample_ts[(slot+RP5_CURVE_MAX-1)%RP5_CURVE_MAX]>10;
            if(stamp<=now && now-stamp<=WINDOW_SEC && !gap){x=width+(int64_t)((uint64_t)stamp*width/WINDOW_SEC)-scroll;rx=(uint64_t)c->down[slot]*8/1000;tx=(uint64_t)c->up[slot]*8/1000;}
        }
        rx_x[i]=tx_x[i]=x;rx_y[i]=rx;tx_y[i]=tx;
    }
    lv_chart_set_range(a->obj,LV_CHART_AXIS_PRIMARY_Y,0,scale);
    if(valid && c->n>1)lv_obj_add_flag(a->empty,LV_OBJ_FLAG_HIDDEN);
    else {lv_obj_remove_flag(a->empty,LV_OBJ_FLAG_HIDDEN);text(a->empty,valid?"Collecting history":"No recent data");}
    lv_chart_refresh(a->obj);
}
// The smallest 1-2-5 step that is >= x (Heckbert §5.3, rounded upward). Upward is the
// whole point: step*SCALE_ROWS is the axis top, so a step rounded down would clip the
// peak that asked for it. 4300 -> 5000, 9800 -> 10000, 1000 -> 1000.
static int32_t nice_step(int32_t x){
    int32_t e=1;while(x>e*10)e*=10;
    int32_t f=(int32_t)(((int64_t)x*10+e-1)/e);            // ceil(mantissa*10), 10..100
    int32_t nf=f<=10?10:f<=20?20:f<=50?50:100;
    int32_t n=(int32_t)((int64_t)nf*e/10);
    return n<SCALE_MIN_STEP?SCALE_MIN_STEP:n;             // the 1 Mbps axis floor
}
// A new peak must never clip, so the axis grows instantly; shrinking is the only
// direction that gets a staircase, because it moves every historical vertex and a
// per-frame rescale reads as the whole curve breathing.
static void scale_request(int32_t want,uint64_t now_ms){
    if(want==animation_scale)return;
    if(want>animation_scale){animation_scale=want;scale_step_index=SCALE_STEPS;return;}
    scale_from=animation_scale;scale_to=want;scale_step_index=0;scale_next_ms=now_ms+100;
}

void rp5_view_refresh(uint64_t now_ms,bool wifi,const char *local_ip){
    uint32_t now=now_ms/1000;
    rp5_sys_t s={0};rp5_proxy_t px={0};rp5_gw_t g={0};rp5_curve_t c={0};
    char b[176],r[32],t[32],wr[32],wt[32],u[32],w[32];
    bool live=rp5_get_sys(&s),proxy=rp5_get_proxy(&px),gw=rp5_get_gw(&g);
    // A stale snapshot would otherwise keep showing yesterday's numbers under a
    // "no data" status; drop the values and let every field fall back to "--".
    if(!live)memset(&s,0,sizeof(s));
    if(!proxy)memset(&px,0,sizeof(px));
    if(!gw)memset(&g,0,sizeof(g));
    text(status,live?"Connected":"No data");tint(status,live?GREEN:AMBER);

    // ── Health ──
    chan_sample(&ch_temp,s.temp_valid,(int32_t)lroundf(s.temp_c*10.0f));
    if(s.load_valid)snprintf(b,sizeof(b),"load %.2f / %.2f / %.2f",(double)s.load1,(double)s.load5,(double)s.load15);
    else strcpy(b,"load -- / -- / --");
    text(load,b);
    chan_sample(&ch_cpu,s.cpu_valid,(int32_t)lroundf(s.cpu_pct));
    chan_sample(&ch_mem,s.mem_valid,(int32_t)lroundf(s.mem_pct));
    if(s.uptime_valid)snprintf(b,sizeof(b),"%lud %luh %lum",(unsigned long)(s.uptime_s/86400),(unsigned long)(s.uptime_s/3600%24),(unsigned long)(s.uptime_s/60%60));
    else strcpy(b,"--");
    text(uptime,b);
    if(s.disk_valid)snprintf(b,sizeof(b),"%.0f%%",(double)s.disk_pct);else strcpy(b,"--");
    text(disk,b);
    if(s.freq_valid)snprintf(b,sizeof(b),"%.1f GHz",(double)s.freq_mhz/1000.0);else strcpy(b,"-- GHz");
    text(freq,b);
    if(s.agent_valid)snprintf(b,sizeof(b),"%lus",(unsigned long)s.agent_up_s);else strcpy(b,"--");
    text(agent,b);
    if(s.ex_valid){if(s.ex_ok)snprintf(b,sizeof(b),"Collector ok | %lu ms",(unsigned long)s.ex_ms);else strcpy(b,"Collector unreachable");}
    else strcpy(b,"Collector unknown");
    text(collector,b);tint(collector,s.ex_valid?(s.ex_ok?GREEN:RED):MUTED);

    // ── Proxy ──
    text(px_node,px.node_valid?px.node:"DIRECT");
    // The channel holds display LSB in Q16 inside int32, so it tops out near 32k; a
    // proxy delay above 10 s is already indistinguishable from "gone", so saturate.
    chan_sample(&ch_delay,px.delay_valid,(int32_t)(px.delay_ms>DELAY_MAX_MS?DELAY_MAX_MS:px.delay_ms));
    snprintf(b,sizeof(b),"%lu connections",(unsigned long)px.conns);text(px_conns,b);
    text(px_mode,px.mode[0]?px.mode:"--");
    format_rate(px.down_bps,r,sizeof(r));format_rate(px.up_bps,t,sizeof(t));
    text(px_down,proxy?r:"--");text(px_up,proxy?t:"--");
    if(proxy&&px.down_total){format_bytes(px.down_total,r,sizeof(r));format_bytes(px.up_total,t,sizeof(t));}
    else {strcpy(r,"--");strcpy(t,"--");}
    text(px_in,r);text(px_out,t);

    // ── Gateway ──
    bool rate=gw;
    if(rate){format_rate(g.wan_rx,wr,sizeof(wr));format_rate(g.wan_tx,wt,sizeof(wt));}
    else {strcpy(wr,"--");strcpy(wt,"--");}
    text(rates[0],wr);text(rates[1],wt);
    if(gw){format_rate(g.lan_rx,r,sizeof(r));format_rate(g.lan_tx,t,sizeof(t));
        format_rate(g.px_rx,u,sizeof(u));format_rate(g.px_tx,w,sizeof(w));
        snprintf(b,sizeof(b),"LAN %s / %s   Proxy %s / %s",r,t,u,w);text(lan_line,b);}
    else text(lan_line,"LAN -- / --");
    bool history=rate && rp5_get_curve(&c);
    if(c.ts!=last_chart_ts || history!=last_chart_valid){
        int64_t peak=0;               // the 1 Mbps floor is nice_step's, not a seeded peak
        for(int i=0;i<c.n;i++){
            uint32_t stamp=c.sample_ts[i];
            if(stamp>now || now-stamp>WINDOW_SEC)continue;
            int64_t v=(int64_t)(c.down[i]>c.up[i]?c.down[i]:c.up[i])*8/1000;
            if(v>peak)peak=v;
        }
        int32_t want=history?nice_step((int32_t)((peak+SCALE_ROWS-1)/SCALE_ROWS))*SCALE_ROWS:SCALE_MIN_STEP*SCALE_ROWS;
        // Peaks age out of the 60 s window on their own, so the only extra guard
        // needed is that a shrink must survive one more 1 Hz poll before the whole
        // curve is rescaled; growth is instant so a new peak is never clipped.
        if(want>=animation_scale){scale_pending=0;scale_request(want,now_ms);}
        else if(want==scale_pending){scale_pending=0;scale_request(want,now_ms);}
        else scale_pending=want;
        animation_curve=c;
        render_activity(&traffic,&c,now_ms,animation_scale,history,true);
        scale_label(animation_scale,history);
        last_chart_ts=c.ts;last_chart_valid=history;
    }

    // ── Footer: what the selected page is showing, or why it is empty ──
    if(!live)snprintf(b,sizeof(b),"%s",failure(wifi));
    else if(selected==0)snprintf(b,sizeof(b),"rp5 | up %lus | %s",(unsigned long)s.agent_up_s,local_ip&&local_ip[0]?local_ip:"no ip");
    else if(selected==1)snprintf(b,sizeof(b),"rp5 | agent up %lus | %s",(unsigned long)s.agent_up_s,local_ip&&local_ip[0]?local_ip:"no ip");
    else snprintf(b,sizeof(b),"rp5 | agent up %lus | %lu leases",(unsigned long)s.agent_up_s,(unsigned long)g.leases);
    text(footer,b);
}
void rp5_view_step(int direction){select_view((selected+direction+VIEWS)%VIEWS);}
void rp5_view_home(void){select_view(0);}
int rp5_view_page(void){return selected;}

// Driven at LV_DEF_REFR_PERIOD: value approaches, axis staircase, timeline scroll.
// Sample values are never rewritten here, only their pixel positions and the axis.
void rp5_view_animate(uint64_t now_ms){
    motion_step(now_ms);
    if(scale_step_index<SCALE_STEPS && now_ms>=scale_next_ms){
        scale_next_ms=now_ms+100;
        if(++scale_step_index>=SCALE_STEPS)animation_scale=scale_to;
        else animation_scale=scale_from+(int32_t)((int64_t)(scale_to-scale_from)*scale_step_index/SCALE_STEPS);
        scale_label(animation_scale,last_chart_valid);
    }
    if(last_chart_valid&&selected==2)
        render_activity(&traffic,&animation_curve,now_ms,animation_scale,true,false);
}
