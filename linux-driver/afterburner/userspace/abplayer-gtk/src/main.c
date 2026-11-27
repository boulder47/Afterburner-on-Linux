// SPDX-License-Identifier: GPL-2.0
/*
 * main.c — Afterburner Player: GTK4/libadwaita ProRes player + test tool.
 *
 * Every frame is decoded by the Apple Afterburner card through
 * /dev/afterburner0 (linux-driver/afterburner); this program only parses
 * the container, feeds packets, and converts/displays the card's UYVY
 * output with OpenGL. There is no software ProRes decoder in here.
 */
#define _GNU_SOURCE
#include <adwaita.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "card.h"
#include "decoder.h"
#include "mov.h"
#include "view.h"

#define APP_ID      "io.github.boulder47.AfterburnerPlayer"
#define APP_NAME    "Afterburner Player"
#define APP_VERSION "0.1.0"
#define APP_URL     "https://github.com/boulder47/afterburner-on-linux"
#define GEN_INVALID UINT32_MAX

/* ---- command line ----------------------------------------------------------- */
static struct {
	gboolean simulate, paused, loop, card_speed, panel;
	gint64 frame;
	char *export_png;
	int exit_status;
} opt = { .frame = 0 };

/* ---- state -------------------------------------------------------------------- */
typedef enum { ERR_NONE, ERR_FILE, ERR_CARD } ErrKind;

typedef struct {
	char *path;
	int fd;
	bool sim;
	Card *card;          /* in: current card (may be NULL); out: card to keep */
	MovInfo mi;
	bool mi_ok;
	ErrKind kind;
	char err[1024];
	volatile int cancel;
	gint done, total;
} LoadJob;

typedef struct {
	GtkWidget *window, *toasts, *tview, *title, *split, *stack;
	GtkWidget *welcome, *welcome_card, *load_page, *loading_bar, *error_page, *error_buttons;
	GtkWidget *overlay, *osd, *badge;
	GtkWidget *controls, *play_btn, *scale, *time_lbl, *dur_lbl, *frame_lbl, *fs_btn;
	GtkWidget *menu_btn;
	VideoView *view;

	/* sidebar values */
	GtkWidget *v_dev, *v_fw, *v_img, *v_state, *v_engine, *v_duty;
	GtkWidget *v_t_back, *v_t_die, *v_t_face, *v_t_bottom;
	GtkWidget *r_file, *v_codec, *v_size, *v_fps, *v_frames, *v_colour, *v_scan, *v_buf;
	GtkWidget *v_decoded, *v_dec_ms, *v_copy_ms, *v_upload_ms, *v_rates, *v_shown, *v_ladder,
		  *v_hist, *v_gl;

	Card *card;
	MovInfo mi;
	bool have_mi;
	int fd;
	char *path;
	Decoder *dec;
	int matrix;          /* effective matrix in use */

	/* presenter */
	Frame *cur;
	uint64_t target_index;
	uint32_t pres_gen;
	bool playing;
	guint tick_id;
	gint64 anchor_us, last_tick_us;
	uint64_t anchor_seq;
	uint64_t shown, dropped, late, upload_sum_ns, last_upload_ns;
	uint64_t shown_win;
	gint64 win_t0;
	double display_fps;
	uint32_t last_duty;
	char last_error[600];

	/* lifecycle */
	LoadJob *job;
	bool loading, closing, close_pending, exported;
	int bg_jobs;
	guint load_poll_id, stats_id, hide_id;
	gint wake_pending;
} Player;

static Player *player_of(gpointer win) { return g_object_get_data(G_OBJECT(win), "player"); }

static void toast(Player *p, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void toast(Player *p, const char *fmt, ...)
{
	va_list ap;
	char *msg;
	AdwToast *t;

	va_start(ap, fmt);
	msg = g_strdup_vprintf(fmt, ap);
	va_end(ap);
	t = adw_toast_new(msg);
	adw_toast_set_use_markup(t, FALSE); /* messages carry paths and shell snippets */
	adw_toast_set_timeout(t, 5);
	adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(p->toasts), t);
	g_free(msg);
}

static void set_action_enabled(Player *p, const char *name, bool on)
{
	GAction *a = g_action_map_lookup_action(G_ACTION_MAP(p->window), name);

	if (a)
		g_simple_action_set_enabled(G_SIMPLE_ACTION(a), on);
}

static void set_action_state(Player *p, const char *name, GVariant *v)
{
	GAction *a = g_action_map_lookup_action(G_ACTION_MAP(p->window), name);

	if (a)
		g_simple_action_set_state(G_SIMPLE_ACTION(a), v);
	else
		g_variant_unref(g_variant_ref_sink(v));
}

static bool action_bool(Player *p, const char *name)
{
	GVariant *s = g_action_group_get_action_state(G_ACTION_GROUP(p->window), name);
	bool r = s && g_variant_get_boolean(s);

	if (s)
		g_variant_unref(s);
	return r;
}

/* ---- formatting ---------------------------------------------------------------- */

static double frame_time(const Player *p, uint64_t idx)
{
	if (p->mi.frame_delta)
		return (double)idx * p->mi.frame_delta / p->mi.timescale;
	return p->mi.fps > 0 ? (double)idx / p->mi.fps : 0;
}

static void fmt_time(double s, char *buf, size_t n)
{
	long ms = lround(s * 1000.0);
	long h = ms / 3600000, m = ms / 60000 % 60, sec = ms / 1000 % 60, milli = ms % 1000;

	if (h)
		snprintf(buf, n, "%ld:%02ld:%02ld.%03ld", h, m, sec, milli);
	else
		snprintf(buf, n, "%02ld:%02ld.%03ld", m, sec, milli);
}

static void set_label(GtkWidget *l, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void set_label(GtkWidget *l, const char *fmt, ...)
{
	va_list ap;
	char buf[512];

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (ADW_IS_ACTION_ROW(l)) { /* sidebar property rows carry the value as subtitle */
		if (g_strcmp0(adw_action_row_get_subtitle(ADW_ACTION_ROW(l)), buf))
			adw_action_row_set_subtitle(ADW_ACTION_ROW(l), buf);
	} else if (g_strcmp0(gtk_label_get_text(GTK_LABEL(l)), buf)) {
		gtk_label_set_text(GTK_LABEL(l), buf);
	}
}

static double ms(uint64_t ns) { return (double)ns / 1e6; }

/* ---- OSD + position ------------------------------------------------------------ */

static void update_osd(Player *p)
{
	DecoderStats ds = { 0 };
	CardCounters cc = { 0 };
	char t[32], buf[1024];

	if (!p->have_mi || !p->dec)
		return;
	decoder_stats(p->dec, &ds);
	if (p->card)
		card_counters(p->card, &cc);
	fmt_time(frame_time(p, p->cur ? p->cur->index : p->target_index), t, sizeof(t));
	snprintf(buf, sizeof(buf),
		 "frame %" PRIu64 " / %" PRIu64 "   %s%s%s\n"
		 "decode %5.2f ms  readback %5.2f ms  upload %5.2f ms\n"
		 "display %5.1f fps  decode %5.1f fps  card busy %u%%\n"
		 "late %" PRIu64 "  dropped %" PRIu64 "  retries %" PRIu64 "  recoveries %" PRIu64
		 "  failed %" PRIu64 "%s%s",
		 p->cur ? p->cur->index : p->target_index, p->mi.n_frames - 1, t,
		 p->playing ? "" : "   ⏸ paused",
		 action_bool(p, "card-speed") ? "   ⚡ card speed" : "",
		 ms(ds.last.decode_ns), ms(ds.last.copy_ns), ms(p->last_upload_ns),
		 p->display_fps, ds.decode_fps, p->last_duty,
		 p->late, p->dropped + ds.recycled, cc.wait_retries, cc.recovers, cc.failed,
		 p->last_error[0] ? "\n⚠ " : "", p->last_error);
	gtk_label_set_text(GTK_LABEL(p->osd), buf);
}

static void update_position(Player *p, uint64_t idx)
{
	char a[32];

	gtk_range_set_value(GTK_RANGE(p->scale), (double)idx);
	fmt_time(frame_time(p, idx), a, sizeof(a));
	set_label(p->time_lbl, "%s", a);
	set_label(p->frame_lbl, "%" PRIu64 " / %" PRIu64, idx, p->mi.n_frames - 1);
}

static void update_play_button(Player *p)
{
	gtk_button_set_icon_name(GTK_BUTTON(p->play_btn), p->playing ? "media-playback-pause-symbolic"
								      : "media-playback-start-symbolic");
	gtk_widget_set_tooltip_text(p->play_btn, p->playing ? "Pause (Space)" : "Play (Space)");
}

/* ---- presenter ----------------------------------------------------------------- */

static void try_export(Player *p)
{
	GdkTexture *tex;
	GError *e = NULL;

	if (!opt.export_png || p->exported || !p->cur || (gint64)p->cur->index != opt.frame)
		return;
	p->exported = true;
	tex = video_view_read_texture(p->view);
	if (!tex) {
		g_printerr("export: no rendered frame (GL: %s)\n",
			   video_view_error(p->view) ? video_view_error(p->view) : "not ready");
		opt.exit_status = 1;
	} else if (!gdk_texture_save_to_png(tex, opt.export_png)) {
		g_printerr("export: cannot write %s\n", opt.export_png);
		opt.exit_status = 1;
	} else {
		DecoderStats ds;

		decoder_stats(p->dec, &ds);
		g_print("exported frame %" PRId64 " (%ux%u, %s, decode %.2f ms on %s) -> %s\n",
			opt.frame, p->mi.width, p->mi.height, mov_matrix_name(p->matrix),
			ms(ds.last.decode_ns), card_name(p->card), opt.export_png);
	}
	g_clear_object(&tex);
	(void)e;
	gtk_window_close(GTK_WINDOW(p->window));
}

static void show_frame(Player *p, Frame *f)
{
	uint64_t up = 0;

	if (video_view_upload(p->view, f->data, &up)) {
		p->last_upload_ns = up;
		p->upload_sum_ns += up;
	}
	if (p->cur)
		decoder_release(p->dec, p->cur);
	p->cur = f;
	p->pres_gen = f->gen;
	p->target_index = f->index;
	p->shown++;
	p->shown_win++;
	update_position(p, f->index);
	update_osd(p);
	try_export(p);
}

static void set_playing(Player *p, bool play);

/* AB_DEBUG_TIMING=1: log every dropped/late presentation with its tick timing */
static bool debug_timing(void)
{
	static int on = -1;

	if (on < 0)
		on = g_getenv("AB_DEBUG_TIMING") != NULL;
	return on;
}

static gboolean on_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
	Player *p = data;
	gint64 now = gdk_frame_clock_get_frame_time(clock);
	unsigned dropped = 0;
	Frame *f;

	(void)w;
	if (!p->dec)
		return G_SOURCE_CONTINUE;

	if (action_bool(p, "card-speed")) {
		/* benchmark: show whatever is newest, decoder never waits on us */
		f = decoder_take(p->dec, p->pres_gen, UINT64_MAX, &dropped);
		if (f) {
			p->dropped += dropped;
			show_frame(p, f);
		}
	} else if (p->pres_gen == GEN_INVALID || !p->cur) {
		f = decoder_take(p->dec, p->pres_gen, UINT64_MAX, &dropped);
		if (f) {
			show_frame(p, f);
			p->anchor_us = now;
			p->anchor_seq = f->seq;
		}
	} else {
		double el = (double)(now - p->anchor_us) / 1e6;
		uint64_t target = p->anchor_seq + (uint64_t)(el * p->mi.fps + 0.25);
		uint32_t gen = p->pres_gen;

		f = decoder_take(p->dec, gen, target, &dropped);
		if (f) {
			bool reanchor = f->gen != gen; /* seek while playing */
			bool late = false;

			if (!reanchor && f->seq < target) {
				p->late++; /* decoder could not keep up: re-time from now */
				reanchor = late = true;
			}
			if ((dropped || late) && debug_timing())
				g_printerr("[timing] frame %" PRIu64 ": tick gap %.1f ms, %.3f s since anchor, "
					   "target seq %" PRIu64 ", shown seq %" PRIu64 ", dropped %u%s\n",
					   f->index, (double)(now - p->last_tick_us) / 1000.0, el, target,
					   f->seq, dropped, late ? ", LATE" : "");
			p->dropped += dropped;
			show_frame(p, f);
			if (reanchor) {
				p->anchor_us = now;
				p->anchor_seq = f->seq;
			}
		}
	}
	if (!f && decoder_drained(p->dec)) {
		set_playing(p, false); /* end of clip */
		update_osd(p);
	}
	p->last_tick_us = now;
	return G_SOURCE_CONTINUE;
}

static void set_playing(Player *p, bool play)
{
	if (!p->dec) {
		p->playing = false;
		return;
	}
	if (play && !action_bool(p, "loop") && p->cur && p->cur->index + 1 >= p->mi.n_frames)
		decoder_seek(p->dec, 0); /* play at the end restarts */
	p->playing = play;
	decoder_set_playing(p->dec, play);
	if (play) {
		p->pres_gen = GEN_INVALID; /* re-anchor on the next frame */
		if (!p->tick_id)
			p->tick_id = gtk_widget_add_tick_callback(video_view_widget(p->view), on_tick, p, NULL);
	} else if (p->tick_id) {
		gtk_widget_remove_tick_callback(video_view_widget(p->view), p->tick_id);
		p->tick_id = 0;
	}
	update_play_button(p);
}

static void seek_to(Player *p, int64_t idx, bool pause)
{
	if (!p->dec)
		return;
	if (idx < 0)
		idx = 0;
	if ((uint64_t)idx >= p->mi.n_frames)
		idx = (int64_t)p->mi.n_frames - 1;
	if (pause && p->playing)
		set_playing(p, false);
	p->target_index = (uint64_t)idx;
	p->last_error[0] = '\0';
	decoder_seek(p->dec, (uint64_t)idx);
	update_position(p, (uint64_t)idx);
}

/* decode thread -> main loop */
static gboolean on_wake(gpointer win)
{
	Player *p = player_of(win);
	DecoderEvent ev;

	if (!p || p->closing || !p->dec)
		return G_SOURCE_REMOVE;
	g_atomic_int_set(&p->wake_pending, 0);

	while (decoder_pop_event(p->dec, &ev)) {
		if (ev.type == DECODER_EV_ERROR) {
			snprintf(p->last_error, sizeof(p->last_error), "frame %" PRIu64 ": %s",
				 ev.frame, ev.msg);
			set_playing(p, false);
			toast(p, "Decode failed at frame %" PRIu64 " — %s", ev.frame, ev.msg);
			if (opt.export_png) {
				g_printerr("decode failed at frame %" PRIu64 ": %s\n", ev.frame, ev.msg);
				opt.exit_status = 1;
				gtk_window_close(GTK_WINDOW(p->window));
				return G_SOURCE_REMOVE;
			}
			update_osd(p);
		}
	}
	if (!p->playing) {
		/* paused: only seek/step results (new generation) are shown */
		Frame *f = decoder_take(p->dec, p->pres_gen, 0, NULL);

		if (f)
			show_frame(p, f);
	}
	return G_SOURCE_REMOVE;
}

static void decoder_wake(void *ud)
{
	Player *p = player_of(ud);

	if (p && !g_atomic_int_exchange(&p->wake_pending, 1))
		g_idle_add_full(G_PRIORITY_DEFAULT, on_wake, g_object_ref(ud), g_object_unref);
}

static void on_view_realized(GtkWidget *area, gpointer data)
{
	Player *p = data;

	(void)area;
	if (video_view_error(p->view)) {
		toast(p, "OpenGL unavailable: %s", video_view_error(p->view));
		return;
	}
	if (p->cur && video_view_upload(p->view, p->cur->data, NULL))
		try_export(p);
}

/* ---- sidebar / stats ----------------------------------------------------------- */

static void sysfs_label(GtkWidget *l, const char *attr, const char *suffix)
{
	char buf[64];

	if (card_sysfs_read(attr, buf, sizeof(buf)) == 0)
		set_label(l, "%s%s", buf, suffix);
	else
		set_label(l, "—");
}

static void update_card_info(Player *p)
{
	bool sim = p->card && card_is_sim(p->card);
	bool present = access(AB_DEVICE_PATH, F_OK) == 0;
	char fw[32];

	if (p->card) {
		CardStatus st;

		set_label(p->v_dev, "%s", card_name(p->card));
		if (card_status(p->card, &st) == 0) {
			if (sim)
				set_label(p->v_fw, "simulated");
			else
				set_label(p->v_fw, "%u.%u", st.fw_version >> 16 & 0xFFFF, st.fw_version & 0xFFFF);
			set_label(p->v_state, "%s, %s", st.ready ? "ready" : "not ready",
				  st.dec_enable ? "enabled" : "stopped");
			set_label(p->v_engine, "0x%08x", st.engine_ctrl);
		}
		if (card_get_percent(p->card, &p->last_duty) == 0)
			set_label(p->v_duty, "%u %%", p->last_duty);
	} else {
		set_label(p->v_dev, present ? AB_DEVICE_PATH " (idle)" : "not found");
		if (card_sysfs_read("fw_version", fw, sizeof(fw)) == 0)
			set_label(p->v_fw, "%s", fw);
		else
			set_label(p->v_fw, "—");
		if (card_sysfs_read("decoder_ready", fw, sizeof(fw)) == 0)
			set_label(p->v_state, "%s", fw[0] == '1' ? "ready" : "not ready");
		else
			set_label(p->v_state, "—");
		set_label(p->v_engine, "—");
		set_label(p->v_duty, "—");
	}
	if (sim) {
		set_label(p->v_img, "—");
		set_label(p->v_t_back, "—");
		set_label(p->v_t_die, "—");
		set_label(p->v_t_face, "—");
		set_label(p->v_t_bottom, "—");
	} else {
		sysfs_label(p->v_img, "image_type", "");
		sysfs_label(p->v_t_back, "temp_backcard_c", " °C");
		sysfs_label(p->v_t_die, "temp_fpga_die_c", " °C");
		sysfs_label(p->v_t_face, "temp_faceplate_c", " °C");
		sysfs_label(p->v_t_bottom, "temp_bottom_c", " °C");
	}

	/* welcome page hint */
	if (p->card)
		set_label(p->welcome_card, "Card: %s", card_name(p->card));
	else if (action_bool(p, "simulate"))
		set_label(p->welcome_card, "Simulated card selected — no hardware will be used");
	else if (present && card_sysfs_read("fw_version", fw, sizeof(fw)) == 0)
		set_label(p->welcome_card, "Card found: " AB_DEVICE_PATH " · firmware %s", fw);
	else if (present)
		set_label(p->welcome_card, "Card found: " AB_DEVICE_PATH);
	else
		set_label(p->welcome_card, "No " AB_DEVICE_PATH " — load the driver, or choose "
			  "Card ▸ Use Simulated Card from the menu");
}

static void update_session_info(Player *p)
{
	DecoderStats ds = { 0 };
	CardCounters cc = { 0 };

	if (p->dec)
		decoder_stats(p->dec, &ds);
	if (p->card)
		card_counters(p->card, &cc);
	set_label(p->v_decoded, "%" PRIu64 " ok · %" PRIu64 " submitted", ds.decoded, cc.submitted);
	if (ds.decoded) {
		set_label(p->v_dec_ms, "%.2f / %.2f / %.2f ms", ms(ds.last.decode_ns),
			  ms(ds.sum_decode_ns / ds.decoded), ms(ds.max_decode_ns));
		set_label(p->v_copy_ms, "%.2f ms read · %.2f ms readback", ms(ds.sum_read_ns / ds.decoded),
			  ms(ds.sum_copy_ns / ds.decoded));
	} else {
		set_label(p->v_dec_ms, "—");
		set_label(p->v_copy_ms, "—");
	}
	set_label(p->v_upload_ms, p->shown ? "%.2f ms" : "—", ms(p->upload_sum_ns / (p->shown ? p->shown : 1)));
	set_label(p->v_rates, "%.1f display · %.1f decode", p->display_fps, ds.decode_fps);
	set_label(p->v_shown, "%" PRIu64 " shown · %" PRIu64 " dropped · %" PRIu64 " late", p->shown,
		  p->dropped + ds.recycled, p->late);
	set_label(p->v_ladder, "%" PRIu64 " retries · %" PRIu64 " recoveries · %" PRIu64 " failed",
		  cc.wait_retries, cc.recovers, cc.failed);
	set_label(p->v_hist, "<2:%" PRIu64 " 2–5:%" PRIu64 " 5–10:%" PRIu64 " 10–20:%" PRIu64
		  " 20–50:%" PRIu64 " ≥50:%" PRIu64, ds.hist[0], ds.hist[1], ds.hist[2], ds.hist[3],
		  ds.hist[4], ds.hist[5]);
	set_label(p->v_gl, "%s", video_view_renderer(p->view));
}

static void update_clip_info(Player *p)
{
	const MovInfo *m = &p->mi;
	char dur[32];

	if (!p->have_mi) {
		adw_action_row_set_subtitle(ADW_ACTION_ROW(p->r_file), "—");
		for (GtkWidget **l = (GtkWidget *[]){ p->v_codec, p->v_size, p->v_fps, p->v_frames,
						     p->v_colour, p->v_scan, p->v_buf, NULL }; *l; l++)
			set_label(*l, "—");
		return;
	}
	adw_action_row_set_subtitle(ADW_ACTION_ROW(p->r_file), p->path);
	set_label(p->v_codec, "%s (%s)%s", m->profile, m->fourcc,
		  m->support == PRORES_UNTESTED ? " · untested on card" : "");
	if (m->par_num != m->par_den)
		set_label(p->v_size, "%u × %u · PAR %u:%u", m->width, m->height, m->par_num, m->par_den);
	else
		set_label(p->v_size, "%u × %u", m->width, m->height);
	if (m->frame_delta)
		set_label(p->v_fps, "%.3f fps (%u/%u)", m->fps, m->timescale, m->frame_delta);
	else
		set_label(p->v_fps, "%.3f fps (variable)", m->fps);
	fmt_time((double)m->duration_ts / m->timescale, dur, sizeof(dur));
	set_label(p->v_frames, "%" PRIu64 " · %s", m->n_frames, dur);
	set_label(p->v_colour, "%s%s · prim %s · trc %s%s", mov_matrix_name(p->matrix),
		  action_bool(p, "full-range") ? " full" : " limited",
		  mov_primaries_name(m->has_colr ? m->primaries : m->hdr_primaries),
		  mov_transfer_name(m->has_colr ? m->transfer : m->hdr_transfer),
		  m->has_colr ? "" : " (frame hdr)");
	set_label(p->v_scan, "%s", m->interlace == 1 ? "interlaced, top field first"
				 : m->interlace == 2 ? "interlaced, bottom field first" : "progressive");
	set_label(p->v_buf, "in %u KiB · out %.1f MiB (%u × %u rows)", m->in_size / 1024,
		  m->out_len / 1048576.0, m->out_stride, m->out_rows);
}

static gboolean on_stats(gpointer data)
{
	Player *p = data;
	gint64 now = g_get_monotonic_time();

	if (p->win_t0) {
		double dt = (double)(now - p->win_t0) / 1e6;

		p->display_fps = dt > 0 ? (double)p->shown_win / dt : 0;
	}
	p->shown_win = 0;
	p->win_t0 = now;
	update_card_info(p);
	update_session_info(p);
	update_osd(p);
	return G_SOURCE_CONTINUE;
}

/* ---- pages --------------------------------------------------------------------- */

static void show_page(Player *p, const char *name)
{
	bool player = !strcmp(name, "player");

	gtk_stack_set_visible_child_name(GTK_STACK(p->stack), name);
	gtk_widget_set_visible(p->controls, player);
	if (player)
		gtk_widget_grab_focus(video_view_widget(p->view));
}

static void clear_children(GtkWidget *box)
{
	GtkWidget *c;

	while ((c = gtk_widget_get_first_child(box)))
		gtk_box_remove(GTK_BOX(box), c);
}

static GtkWidget *pill(const char *label, const char *action, bool suggested)
{
	GtkWidget *b = gtk_button_new_with_mnemonic(label);

	gtk_widget_add_css_class(b, "pill");
	if (suggested)
		gtk_widget_add_css_class(b, "suggested-action");
	gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
	gtk_widget_set_halign(b, GTK_ALIGN_CENTER);
	return b;
}

static void status_set_text(GtkWidget *page, const char *text)
{
	char *esc = g_markup_escape_text(text, -1); /* descriptions are always markup */

	adw_status_page_set_description(ADW_STATUS_PAGE(page), esc);
	g_free(esc);
}

static void show_error(Player *p, ErrKind kind, const char *msg)
{
	adw_status_page_set_title(ADW_STATUS_PAGE(p->error_page),
				  kind == ERR_CARD ? "Afterburner Card Unavailable" : "Can’t Play This File");
	status_set_text(p->error_page, msg);
	clear_children(p->error_buttons);
	if (kind == ERR_CARD) {
		gtk_box_append(GTK_BOX(p->error_buttons), pill("_Try Again", "win.reload", true));
		gtk_box_append(GTK_BOX(p->error_buttons), pill("Use _Simulated Card", "win.use-sim", false));
	} else {
		gtk_box_append(GTK_BOX(p->error_buttons), pill("_Open Another File…", "win.open", true));
	}
	show_page(p, "error");
	adw_window_title_set_subtitle(ADW_WINDOW_TITLE(p->title), "");
}

/* ---- open / close -------------------------------------------------------------- */

/* abplayer-style summary on stdout when a clip is closed — for terminal runs */
static void print_session_stats(Player *p)
{
	DecoderStats ds;
	CardCounters cc = { 0 };

	if (!p->dec || !p->have_mi)
		return;
	decoder_stats(p->dec, &ds);
	if (p->card)
		card_counters(p->card, &cc);
	g_print("== %s [%s]: %" PRIu64 " decoded (%" PRIu64 " submitted, %" PRIu64 " failed), "
		"%" PRIu64 " shown, %" PRIu64 " dropped, %" PRIu64 " late, retries=%" PRIu64
		" recoveries=%" PRIu64 "\n", p->path, card_name(p->card), ds.decoded, cc.submitted,
		cc.failed, p->shown, p->dropped + ds.recycled, p->late, cc.wait_retries, cc.recovers);
	if (ds.decoded)
		g_print("   decode avg/max %.2f/%.2f ms, read %.2f ms, readback %.2f ms, upload %.2f ms | "
			"<2ms:%" PRIu64 " 2-5:%" PRIu64 " 5-10:%" PRIu64 " 10-20:%" PRIu64 " 20-50:%" PRIu64
			" >=50:%" PRIu64 "\n", ms(ds.sum_decode_ns / ds.decoded), ms(ds.max_decode_ns),
			ms(ds.sum_read_ns / ds.decoded), ms(ds.sum_copy_ns / ds.decoded),
			ms(p->upload_sum_ns / (p->shown ? p->shown : 1)), ds.hist[0], ds.hist[1], ds.hist[2],
			ds.hist[3], ds.hist[4], ds.hist[5]);
}

static void close_clip(Player *p)
{
	print_session_stats(p);
	set_playing(p, false);
	if (p->dec) {
		if (p->cur)
			decoder_release(p->dec, p->cur);
		p->cur = NULL;
		decoder_free(p->dec);
		p->dec = NULL;
	}
	if (p->fd >= 0)
		close(p->fd);
	p->fd = -1;
	if (p->have_mi)
		mov_free(&p->mi);
	p->have_mi = false;
	p->last_error[0] = '\0';
	video_view_clear(p->view);
	for (const char *const *a = (const char *const[]){ "play-pause", "step-forward", "step-back",
			"go-start", "go-end", "save-png", "save-raw", NULL }; *a; a++)
		set_action_enabled(p, *a, false);
}

static void apply_colour(Player *p)
{
	GVariant *s;
	const char *m;

	if (!p->have_mi)
		return;
	s = g_action_group_get_action_state(G_ACTION_GROUP(p->window), "matrix");
	m = g_variant_get_string(s, NULL);
	p->matrix = !strcmp(m, "601") ? 6 : !strcmp(m, "709") ? 1 : !strcmp(m, "2020") ? 9
		    : mov_effective_matrix(&p->mi);
	g_variant_unref(s);
	video_view_set_colour(p->view, p->matrix, action_bool(p, "full-range"));
	update_clip_info(p);
}

static void job_free(gpointer data)
{
	LoadJob *j = data;

	if (j->fd >= 0)
		close(j->fd);
	if (j->mi_ok)
		mov_free(&j->mi);
	if (j->card)
		card_close(j->card);
	g_free(j->path);
	g_free(j);
}

static void load_progress(uint64_t done, uint64_t total, void *ud)
{
	LoadJob *j = ud;

	g_atomic_int_set(&j->done, (gint)MIN(done, (uint64_t)G_MAXINT));
	g_atomic_int_set(&j->total, (gint)MIN(total, (uint64_t)G_MAXINT));
}

static void load_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
	LoadJob *j = data;
	CardGeometry g;

	(void)src;
	(void)c;
	j->fd = open(j->path, O_RDONLY | O_CLOEXEC);
	if (j->fd < 0) {
		j->kind = ERR_FILE;
		snprintf(j->err, sizeof(j->err), "Cannot open %s: %s", j->path, g_strerror(errno));
		goto done;
	}
	if (mov_parse(j->fd, &j->mi, j->err, sizeof(j->err))) {
		j->kind = ERR_FILE;
		goto done;
	}
	j->mi_ok = true;
	if (mov_selfcheck(j->fd, &j->mi, load_progress, j, &j->cancel, j->err, sizeof(j->err))) {
		j->kind = ERR_FILE;
		goto done;
	}
	if (j->card && card_is_sim(j->card) != j->sim) {
		card_close(j->card);
		j->card = NULL;
	}
	if (!j->card) {
		j->card = j->sim ? card_open_sim(j->err, sizeof(j->err))
				 : card_open_hw(AB_DEVICE_PATH, j->err, sizeof(j->err));
		if (!j->card) {
			j->kind = ERR_CARD;
			goto done;
		}
	}
	g = (CardGeometry){
		.width = j->mi.width, .height = j->mi.height,
		.stride = j->mi.out_stride, .rows = j->mi.out_rows,
		.in_size = j->mi.in_size, .out_len = j->mi.out_len,
		.matrix = mov_effective_matrix(&j->mi),
	};
	if (card_bind(j->card, &g, j->err, sizeof(j->err)))
		j->kind = ERR_CARD;
done:
	g_task_return_boolean(task, j->kind == ERR_NONE);
}

static void finish_close(Player *p);

static void on_load_done(GObject *src, GAsyncResult *res, gpointer ud)
{
	Player *p = player_of(src);
	LoadJob *j = g_task_get_task_data(G_TASK(res));
	char sub[256];

	(void)ud;
	p->loading = false;
	p->job = NULL;
	if (p->load_poll_id) {
		g_source_remove(p->load_poll_id);
		p->load_poll_id = 0;
	}
	p->card = j->card; /* keep the card open whatever happened to the file */
	j->card = NULL;
	if (p->close_pending) {
		finish_close(p);
		return;
	}
	if (j->kind != ERR_NONE) {
		if (opt.export_png) {
			g_printerr("%s\n", j->err);
			opt.exit_status = 1;
			gtk_window_close(GTK_WINDOW(p->window));
			return;
		}
		show_error(p, j->kind, j->err);
		update_card_info(p);
		return;
	}

	p->mi = j->mi;
	j->mi_ok = false;
	p->have_mi = true;
	p->fd = j->fd;
	j->fd = -1;
	p->shown = p->dropped = p->late = p->upload_sum_ns = p->last_upload_ns = 0;
	p->pres_gen = GEN_INVALID;
	p->exported = false;

	set_action_state(p, "full-range", g_variant_new_boolean(p->mi.full_range));
	video_view_set_format(p->view, p->mi.width, p->mi.height, p->mi.out_stride,
			      (double)p->mi.par_num / p->mi.par_den);
	apply_colour(p);
	gtk_range_set_range(GTK_RANGE(p->scale), 0, (double)(p->mi.n_frames > 1 ? p->mi.n_frames - 1 : 1));
	{
		char d[32];

		fmt_time((double)p->mi.duration_ts / p->mi.timescale, d, sizeof(d));
		set_label(p->dur_lbl, "%s", d);
	}
	{
		char *base = g_path_get_basename(p->path);

		adw_window_title_set_title(ADW_WINDOW_TITLE(p->title), base);
		g_free(base);
	}
	snprintf(sub, sizeof(sub), "%s%s · %u×%u · %.3f fps", card_is_sim(p->card) ? "SIMULATED · " : "",
		 p->mi.profile, p->mi.width, p->mi.height, p->mi.fps);
	adw_window_title_set_subtitle(ADW_WINDOW_TITLE(p->title), sub);
	gtk_widget_set_visible(p->badge, card_is_sim(p->card));
	update_clip_info(p);

	p->dec = decoder_new(p->card, &p->mi, p->fd, decoder_wake, p->window);
	if (!p->dec) {
		show_error(p, ERR_FILE, "Out of memory allocating frame buffers");
		close_clip(p);
		return;
	}
	decoder_set_loop(p->dec, action_bool(p, "loop"));
	decoder_set_free_run(p->dec, action_bool(p, "card-speed"));
	for (const char *const *a = (const char *const[]){ "play-pause", "step-forward", "step-back",
			"go-start", "go-end", "save-png", "save-raw", NULL }; *a; a++)
		set_action_enabled(p, *a, true);

	show_page(p, "player");
	if (p->mi.support == PRORES_UNTESTED)
		toast(p, "%s has not been verified on the card yet — results are a test data point",
		      p->mi.profile);
	if (p->mi.interlace)
		toast(p, "Interlaced clip: card output is shown as delivered (field handling untested)");
	seek_to(p, opt.frame, false);
	set_playing(p, !(opt.paused || opt.export_png));
	update_card_info(p);
	update_osd(p);
}

static gboolean on_load_poll(gpointer data)
{
	Player *p = data;
	gint done, total;

	if (!p->job)
		return G_SOURCE_CONTINUE;
	done = g_atomic_int_get(&p->job->done);
	total = g_atomic_int_get(&p->job->total);
	if (total > 0) {
		char txt[96];

		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(p->loading_bar), (double)done / total);
		snprintf(txt, sizeof(txt), "frame %d of %d", done, total);
		gtk_progress_bar_set_text(GTK_PROGRESS_BAR(p->loading_bar), txt);
	} else {
		gtk_progress_bar_pulse(GTK_PROGRESS_BAR(p->loading_bar));
	}
	return G_SOURCE_CONTINUE;
}

static void open_path(Player *p, const char *path)
{
	LoadJob *j;
	GTask *task;
	char *base;

	if (p->loading || p->bg_jobs) {
		toast(p, "Busy — wait for the current operation to finish");
		return;
	}
	if (p->path != path) {
		g_free(p->path);
		p->path = g_strdup(path);
	}
	close_clip(p);

	j = g_new0(LoadJob, 1);
	j->fd = -1;
	j->path = g_strdup(path);
	j->sim = action_bool(p, "simulate");
	j->card = p->card; /* ownership moves to the job until it completes */
	p->card = NULL;
	p->job = j;
	p->loading = true;

	base = g_path_get_basename(path);
	status_set_text(p->load_page, base);
	adw_window_title_set_title(ADW_WINDOW_TITLE(p->title), base);
	adw_window_title_set_subtitle(ADW_WINDOW_TITLE(p->title), "Checking frames…");
	g_free(base);
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(p->loading_bar), 0);
	gtk_progress_bar_set_text(GTK_PROGRESS_BAR(p->loading_bar), "reading movie header");
	show_page(p, "loading");
	p->load_poll_id = g_timeout_add(100, on_load_poll, p);

	task = g_task_new(p->window, NULL, on_load_done, NULL);
	g_task_set_task_data(task, j, job_free);
	g_task_run_in_thread(task, load_thread);
	g_object_unref(task);
}

static void open_file(Player *p, GFile *file)
{
	char *path = g_file_get_path(file);

	if (!path) {
		toast(p, "Only local files can be played");
		return;
	}
	open_path(p, path);
	g_free(path);
}

static void finish_close(Player *p)
{
	p->closing = true;
	if (p->stats_id)
		g_source_remove(p->stats_id);
	if (p->hide_id)
		g_source_remove(p->hide_id);
	if (p->load_poll_id)
		g_source_remove(p->load_poll_id);
	p->stats_id = p->hide_id = p->load_poll_id = 0;
	close_clip(p);
	card_close(p->card); /* STOP + unbind: card idles quiesced */
	p->card = NULL;
	gtk_window_destroy(GTK_WINDOW(p->window));
}

static gboolean on_close_request(GtkWindow *w, gpointer data)
{
	Player *p = data;

	(void)w;
	if (p->closing)
		return FALSE;
	if (p->loading || p->bg_jobs) {
		/* a worker owns the card: finish when it returns */
		p->close_pending = true;
		if (p->job)
			p->job->cancel = 1;
		gtk_widget_set_visible(p->window, FALSE);
		return TRUE;
	}
	finish_close(p);
	return TRUE;
}

/* ---- card commands (run off the UI thread; they block on the card lock) ---------- */

typedef enum { CMD_START, CMD_STOP, CMD_RESET } CardCmd;
typedef struct { Card *card; CardCmd cmd; int rc; char err[512]; } CardJob;

static void card_cmd_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
	CardJob *j = data;

	(void)src;
	(void)c;
	j->rc = j->cmd == CMD_START ? card_start(j->card, j->err, sizeof(j->err))
		: j->cmd == CMD_STOP ? card_stop(j->card, j->err, sizeof(j->err))
				     : card_reset(j->card, j->err, sizeof(j->err));
	g_task_return_boolean(task, TRUE);
}

static void on_card_cmd_done(GObject *src, GAsyncResult *res, gpointer ud)
{
	Player *p = player_of(src);
	CardJob *j = g_task_get_task_data(G_TASK(res));

	(void)ud;
	p->bg_jobs--;
	if (p->close_pending && !p->loading && !p->bg_jobs) {
		finish_close(p);
		return;
	}
	if (j->rc)
		toast(p, "%s", j->err);
	else if (j->cmd == CMD_STOP)
		toast(p, "Decoder stopped — it is started again automatically on the next frame");
	else if (j->cmd == CMD_START)
		toast(p, "Decoder started");
	else
		toast(p, "Card reset and DMA regions re-bound");
	update_card_info(p);
}

static void run_card_cmd(Player *p, CardCmd cmd)
{
	CardJob *j;
	GTask *task;

	if (!p->card || p->loading) {
		toast(p, "No card is open — open a clip first");
		return;
	}
	j = g_new0(CardJob, 1);
	j->card = p->card;
	j->cmd = cmd;
	p->bg_jobs++;
	task = g_task_new(p->window, NULL, on_card_cmd_done, NULL);
	g_task_set_task_data(task, j, g_free);
	g_task_run_in_thread(task, card_cmd_thread);
	g_object_unref(task);
}

static void on_reset_confirm(AdwAlertDialog *d, const char *response, gpointer data)
{
	(void)d;
	if (!g_strcmp0(response, "reset"))
		run_card_cmd(data, CMD_RESET);
}

/* ---- actions ------------------------------------------------------------------- */

static void on_open_done(GObject *src, GAsyncResult *res, gpointer win)
{
	Player *p = player_of(win);
	GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);

	if (file && p && !p->closing)
		open_file(p, file);
	g_clear_object(&file);
	g_object_unref(win);
}

static void act_open(GSimpleAction *a, GVariant *v, gpointer data)
{
	Player *p = data;
	GtkFileDialog *dlg = gtk_file_dialog_new();
	GtkFileFilter *f = gtk_file_filter_new(), *all = gtk_file_filter_new();
	GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);

	(void)a;
	(void)v;
	gtk_file_filter_set_name(f, "QuickTime / MP4 video");
	gtk_file_filter_add_mime_type(f, "video/quicktime");
	gtk_file_filter_add_mime_type(f, "video/mp4");
	gtk_file_filter_add_suffix(f, "mov");
	gtk_file_filter_add_suffix(f, "mp4");
	gtk_file_filter_add_suffix(f, "m4v");
	gtk_file_filter_set_name(all, "All files");
	gtk_file_filter_add_pattern(all, "*");
	g_list_store_append(filters, f);
	g_list_store_append(filters, all);
	gtk_file_dialog_set_title(dlg, "Open ProRes Clip");
	gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
	gtk_file_dialog_set_default_filter(dlg, f);
	gtk_file_dialog_open(dlg, GTK_WINDOW(p->window), NULL, on_open_done, g_object_ref(p->window));
	g_object_unref(filters);
	g_object_unref(f);
	g_object_unref(all);
	g_object_unref(dlg);
}

static void act_reload(GSimpleAction *a, GVariant *v, gpointer data)
{
	Player *p = data;

	(void)a;
	(void)v;
	if (p->path)
		open_path(p, p->path);
}

static void act_use_sim(GSimpleAction *a, GVariant *v, gpointer data)
{
	Player *p = data;

	(void)a;
	(void)v;
	set_action_state(p, "simulate", g_variant_new_boolean(TRUE));
	if (p->path)
		open_path(p, p->path);
}

static void act_play_pause(GSimpleAction *a, GVariant *v, gpointer data)
{
	Player *p = data;

	(void)a;
	(void)v;
	set_playing(p, !p->playing);
	update_osd(p);
}

static void act_step_fwd(GSimpleAction *a, GVariant *v, gpointer p)
{
	(void)a; (void)v;
	seek_to(p, (int64_t)((Player *)p)->target_index + 1, true);
}

static void act_step_back(GSimpleAction *a, GVariant *v, gpointer p)
{
	(void)a; (void)v;
	seek_to(p, (int64_t)((Player *)p)->target_index - 1, true);
}

static void act_go_start(GSimpleAction *a, GVariant *v, gpointer p)
{
	(void)a; (void)v;
	seek_to(p, 0, false);
}

static void act_go_end(GSimpleAction *a, GVariant *v, gpointer p)
{
	(void)a; (void)v;
	seek_to(p, (int64_t)((Player *)p)->mi.n_frames - 1, true);
}

static void act_fullscreen(GSimpleAction *a, GVariant *v, gpointer data)
{
	Player *p = data;

	(void)a;
	(void)v;
	if (gtk_window_is_fullscreen(GTK_WINDOW(p->window)))
		gtk_window_unfullscreen(GTK_WINDOW(p->window));
	else
		gtk_window_fullscreen(GTK_WINDOW(p->window));
}

static void act_loop(GSimpleAction *a, GVariant *v, gpointer data)
{
	Player *p = data;

	g_simple_action_set_state(a, v);
	if (p->dec)
		decoder_set_loop(p->dec, g_variant_get_boolean(v));
}

static void act_card_speed(GSimpleAction *a, GVariant *v, gpointer data)
{
	Player *p = data;

	g_simple_action_set_state(a, v);
	if (p->dec) {
		decoder_set_free_run(p->dec, g_variant_get_boolean(v));
		p->pres_gen = GEN_INVALID;
	}
	update_osd(p);
}

static void act_osd(GSimpleAction *a, GVariant *v, gpointer data)
{
	Player *p = data;

	g_simple_action_set_state(a, v);
	gtk_widget_set_visible(p->osd, g_variant_get_boolean(v));
}

static void act_sidebar(GSimpleAction *a, GVariant *v, gpointer data)
{
	Player *p = data;

	g_simple_action_set_state(a, v);
	adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(p->split), g_variant_get_boolean(v));
}

static void act_simulate(GSimpleAction *a, GVariant *v, gpointer data)
{
	Player *p = data;

	g_simple_action_set_state(a, v);
	update_card_info(p);
	if (p->have_mi && p->card && card_is_sim(p->card) != g_variant_get_boolean(v))
		open_path(p, p->path); /* re-open on the other backend */
}

static void act_full_range(GSimpleAction *a, GVariant *v, gpointer data)
{
	g_simple_action_set_state(a, v);
	apply_colour(data);
}

static void act_matrix(GSimpleAction *a, GVariant *v, gpointer data)
{
	g_simple_action_set_state(a, v);
	apply_colour(data);
}

static void act_card_start(GSimpleAction *a, GVariant *v, gpointer p)
{
	(void)a; (void)v;
	run_card_cmd(p, CMD_START);
}

static void act_card_stop(GSimpleAction *a, GVariant *v, gpointer p)
{
	(void)a; (void)v;
	run_card_cmd(p, CMD_STOP);
}

static void act_card_reset(GSimpleAction *a, GVariant *v, gpointer data)
{
	Player *p = data;
	AdwDialog *d;

	(void)a;
	(void)v;
	d = adw_alert_dialog_new("Hard Reset the Card?",
				 "Issues a PCIe function-level reset and a full decoder bring-up. "
				 "The driver refuses if the card does not advertise FLR (the Afterburner "
				 "normally does not) — Stop and Start Decoder is the proven recovery path.");
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d), "cancel", "_Cancel", "reset", "_Reset", NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "reset", ADW_RESPONSE_DESTRUCTIVE);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(d), "cancel");
	g_signal_connect(d, "response", G_CALLBACK(on_reset_confirm), p);
	adw_dialog_present(d, p->window);
}

static void act_reset_stats(GSimpleAction *a, GVariant *v, gpointer data)
{
	Player *p = data;

	(void)a;
	(void)v;
	p->shown = p->dropped = p->late = p->upload_sum_ns = 0;
	if (p->dec)
		decoder_reset_stats(p->dec);
	update_session_info(p);
	update_osd(p);
}

/* save dialogs: capture the frame when the user asks, write when they pick a name */
typedef struct { GObject *win; GdkTexture *tex; GBytes *raw; uint64_t frame; } SaveJob;

static void on_save_done(GObject *src, GAsyncResult *res, gpointer data)
{
	SaveJob *s = data;
	Player *p = player_of(s->win);
	GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);

	if (file && p && !p->closing) {
		char *path = g_file_get_path(file);
		GError *e = NULL;

		if (s->tex) {
			if (path && gdk_texture_save_to_png(s->tex, path))
				toast(p, "Saved frame %" PRIu64 " as PNG", s->frame);
			else
				toast(p, "Could not write the PNG");
		} else if (path && g_file_set_contents(path, g_bytes_get_data(s->raw, NULL),
						       (gssize)g_bytes_get_size(s->raw), &e)) {
			toast(p, "Saved frame %" PRIu64 ": %u×%u UYVY [U,Y0,V,Y1], stride %u — "
			      "compare with tests/compare_card_ref.py", s->frame, p->mi.width,
			      p->mi.height, p->mi.out_stride);
		} else {
			toast(p, "Could not write the raw frame: %s", e ? e->message : "no path");
		}
		g_clear_error(&e);
		g_free(path);
	}
	g_clear_object(&file);
	g_clear_object(&s->tex);
	if (s->raw)
		g_bytes_unref(s->raw);
	g_object_unref(s->win);
	g_free(s);
}

static void save_frame(Player *p, bool png)
{
	SaveJob *s;
	GtkFileDialog *dlg;
	char *base, *dot, *name;

	if (!p->cur) {
		toast(p, "No frame to save yet");
		return;
	}
	s = g_new0(SaveJob, 1);
	s->win = g_object_ref(G_OBJECT(p->window));
	s->frame = p->cur->index;
	if (png) {
		s->tex = video_view_read_texture(p->view);
		if (!s->tex) {
			toast(p, "Nothing rendered yet");
			g_object_unref(s->win);
			g_free(s);
			return;
		}
	} else {
		s->raw = g_bytes_new(p->cur->data, (gsize)p->mi.out_stride * p->mi.height);
	}
	base = g_path_get_basename(p->path);
	if ((dot = strrchr(base, '.')))
		*dot = '\0';
	name = g_strdup_printf("%s_frame%06" PRIu64 "%s", base, s->frame,
			       png ? ".png" : "_card_uyvy.yuv");
	dlg = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dlg, png ? "Save Frame as PNG" : "Save Raw Card Output");
	gtk_file_dialog_set_initial_name(dlg, name);
	gtk_file_dialog_save(dlg, GTK_WINDOW(p->window), NULL, on_save_done, s);
	g_object_unref(dlg);
	g_free(name);
	g_free(base);
}

static void act_save_png(GSimpleAction *a, GVariant *v, gpointer p)
{
	(void)a; (void)v;
	save_frame(p, true);
}

static void act_save_raw(GSimpleAction *a, GVariant *v, gpointer p)
{
	(void)a; (void)v;
	save_frame(p, false);
}

static void act_shortcuts(GSimpleAction *a, GVariant *v, gpointer data)
{
	static const char *const keys[][2] = {
		{ "Play / pause", "Space  K" },
		{ "Previous / next frame", "←  →" },
		{ "Back / forward 1 second", "Shift+←  Shift+→" },
		{ "First / last frame", "Home  End" },
		{ "Fullscreen", "F  F11" },
		{ "Leave fullscreen", "Esc" },
		{ "Statistics overlay", "I" },
		{ "Card & statistics panel", "F9" },
		{ "Loop", "L" },
		{ "Run at card speed", "B" },
		{ "Open", "Ctrl+O" },
		{ "Save frame as PNG", "Ctrl+S" },
		{ "Save raw card output", "Ctrl+Shift+S" },
		{ "Quit", "Ctrl+Q" },
	};
	Player *p = data;
	AdwDialog *d = adw_dialog_new();
	GtkWidget *tv = adw_toolbar_view_new(), *page = adw_preferences_page_new();
	GtkWidget *grp = adw_preferences_group_new();

	(void)a;
	(void)v;
	for (size_t i = 0; i < G_N_ELEMENTS(keys); i++) {
		GtkWidget *row = adw_action_row_new(), *k = gtk_label_new(keys[i][1]);

		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), keys[i][0]);
		adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
		gtk_widget_add_css_class(k, "dim-label");
		gtk_widget_add_css_class(k, "monospace");
		adw_action_row_add_suffix(ADW_ACTION_ROW(row), k);
		adw_preferences_group_add(ADW_PREFERENCES_GROUP(grp), row);
	}
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(grp));
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), adw_header_bar_new());
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), page);
	adw_dialog_set_title(d, "Keyboard Shortcuts");
	adw_dialog_set_content_width(d, 440);
	adw_dialog_set_content_height(d, 620);
	adw_dialog_set_child(d, tv);
	adw_dialog_present(d, p->window);
}

static const GActionEntry win_actions[] = {
	{ "open", act_open, NULL, NULL, NULL, { 0 } },
	{ "reload", act_reload, NULL, NULL, NULL, { 0 } },
	{ "use-sim", act_use_sim, NULL, NULL, NULL, { 0 } },
	{ "play-pause", act_play_pause, NULL, NULL, NULL, { 0 } },
	{ "step-forward", act_step_fwd, NULL, NULL, NULL, { 0 } },
	{ "step-back", act_step_back, NULL, NULL, NULL, { 0 } },
	{ "go-start", act_go_start, NULL, NULL, NULL, { 0 } },
	{ "go-end", act_go_end, NULL, NULL, NULL, { 0 } },
	{ "fullscreen", act_fullscreen, NULL, NULL, NULL, { 0 } },
	{ "save-png", act_save_png, NULL, NULL, NULL, { 0 } },
	{ "save-raw", act_save_raw, NULL, NULL, NULL, { 0 } },
	{ "card-start", act_card_start, NULL, NULL, NULL, { 0 } },
	{ "card-stop", act_card_stop, NULL, NULL, NULL, { 0 } },
	{ "card-reset", act_card_reset, NULL, NULL, NULL, { 0 } },
	{ "reset-stats", act_reset_stats, NULL, NULL, NULL, { 0 } },
	{ "shortcuts", act_shortcuts, NULL, NULL, NULL, { 0 } },
	{ "loop", NULL, NULL, "false", act_loop, { 0 } },
	{ "card-speed", NULL, NULL, "false", act_card_speed, { 0 } },
	{ "osd", NULL, NULL, "true", act_osd, { 0 } },
	{ "sidebar", NULL, NULL, "false", act_sidebar, { 0 } },
	{ "simulate", NULL, NULL, "false", act_simulate, { 0 } },
	{ "full-range", NULL, NULL, "false", act_full_range, { 0 } },
	{ "matrix", NULL, "s", "'auto'", act_matrix, { 0 } },
};

/* ---- input --------------------------------------------------------------------- */

static gboolean on_key(GtkEventControllerKey *k, guint key, guint code, GdkModifierType mods,
		       gpointer data)
{
	Player *p = data;
	GtkWindow *w = GTK_WINDOW(p->window);
	bool shift = mods & GDK_SHIFT_MASK;
	double one_sec = p->have_mi ? p->mi.fps : 30;
	const char *act = NULL;

	(void)k;
	(void)code;
	if (mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK))
		return FALSE; /* application accelerators */
	if (adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(p->window)) ||
	    gtk_menu_button_get_active(GTK_MENU_BUTTON(p->menu_btn)))
		return FALSE;

	switch (key) {
	case GDK_KEY_space: case GDK_KEY_k: case GDK_KEY_K: act = "play-pause"; break;
	case GDK_KEY_Left:
		if (shift)
			seek_to(p, (int64_t)p->target_index - (int64_t)lround(one_sec), false);
		else
			act = "step-back";
		break;
	case GDK_KEY_Right:
		if (shift)
			seek_to(p, (int64_t)p->target_index + (int64_t)lround(one_sec), false);
		else
			act = "step-forward";
		break;
	case GDK_KEY_comma: act = "step-back"; break;
	case GDK_KEY_period: act = "step-forward"; break;
	case GDK_KEY_Home: act = "go-start"; break;
	case GDK_KEY_End: act = "go-end"; break;
	case GDK_KEY_f: case GDK_KEY_F: case GDK_KEY_F11: act = "fullscreen"; break;
	case GDK_KEY_i: case GDK_KEY_I: act = "osd"; break;
	case GDK_KEY_l: case GDK_KEY_L: act = "loop"; break;
	case GDK_KEY_b: case GDK_KEY_B: act = "card-speed"; break;
	case GDK_KEY_F9: act = "sidebar"; break;
	case GDK_KEY_Escape:
		if (!gtk_window_is_fullscreen(w))
			return FALSE;
		gtk_window_unfullscreen(w);
		return TRUE;
	default:
		return FALSE;
	}
	if (act)
		g_action_group_activate_action(G_ACTION_GROUP(p->window), act, NULL);
	return TRUE;
}

static gboolean on_scale_change(GtkRange *r, GtkScrollType scroll, double value, gpointer data)
{
	(void)r;
	(void)scroll;
	seek_to(data, (int64_t)llround(value), false);
	return FALSE;
}

static gboolean on_drop(GtkDropTarget *t, const GValue *value, double x, double y, gpointer data)
{
	(void)t;
	(void)x;
	(void)y;
	if (!G_VALUE_HOLDS(value, G_TYPE_FILE))
		return FALSE;
	open_file(data, g_value_get_object(value));
	return TRUE;
}

static gboolean hide_bars(gpointer data)
{
	Player *p = data;

	p->hide_id = 0;
	if (gtk_window_is_fullscreen(GTK_WINDOW(p->window))) {
		adw_toolbar_view_set_reveal_top_bars(ADW_TOOLBAR_VIEW(p->tview), FALSE);
		adw_toolbar_view_set_reveal_bottom_bars(ADW_TOOLBAR_VIEW(p->tview), FALSE);
		gtk_widget_set_cursor_from_name(p->overlay, "none");
	}
	return G_SOURCE_REMOVE;
}

static void on_motion(GtkEventControllerMotion *m, double x, double y, gpointer data)
{
	Player *p = data;
	static double lx = -1, ly = -1;

	(void)m;
	if (!gtk_window_is_fullscreen(GTK_WINDOW(p->window)) || (fabs(x - lx) < 2 && fabs(y - ly) < 2))
		return;
	lx = x;
	ly = y;
	adw_toolbar_view_set_reveal_top_bars(ADW_TOOLBAR_VIEW(p->tview), TRUE);
	adw_toolbar_view_set_reveal_bottom_bars(ADW_TOOLBAR_VIEW(p->tview), TRUE);
	gtk_widget_set_cursor(p->overlay, NULL);
	if (p->hide_id)
		g_source_remove(p->hide_id);
	p->hide_id = g_timeout_add(2000, hide_bars, p);
}

static void on_fullscreen_changed(GObject *o, GParamSpec *ps, gpointer data)
{
	Player *p = data;
	bool fs = gtk_window_is_fullscreen(GTK_WINDOW(o));

	(void)ps;
	gtk_button_set_icon_name(GTK_BUTTON(p->fs_btn), fs ? "view-restore-symbolic" : "view-fullscreen-symbolic");
	adw_toolbar_view_set_reveal_top_bars(ADW_TOOLBAR_VIEW(p->tview), !fs);
	adw_toolbar_view_set_reveal_bottom_bars(ADW_TOOLBAR_VIEW(p->tview), !fs);
	if (!fs) {
		gtk_widget_set_cursor(p->overlay, NULL);
		if (p->hide_id) {
			g_source_remove(p->hide_id);
			p->hide_id = 0;
		}
	}
}

static void on_sidebar_shown(GObject *o, GParamSpec *ps, gpointer data)
{
	(void)ps;
	set_action_state(data, "sidebar",
			 g_variant_new_boolean(adw_overlay_split_view_get_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(o))));
}

/* ---- construction -------------------------------------------------------------- */

static GtkWidget *value_row(GtkWidget *group, const char *title)
{
	GtkWidget *row = adw_action_row_new();

	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE); /* values are plain text */
	adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "—");
	adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), TRUE);
	gtk_widget_add_css_class(row, "property"); /* libadwaita >= 1.6: value emphasised */
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
	return row;
}

static GtkWidget *group(GtkWidget *box, const char *title)
{
	GtkWidget *g = adw_preferences_group_new();

	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g), title);
	gtk_box_append(GTK_BOX(box), g);
	return g;
}

static GtkWidget *build_sidebar(Player *p)
{
	GtkWidget *sw = gtk_scrolled_window_new(), *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
	GtkWidget *g, *reset;

	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_margin_start(box, 12);
	gtk_widget_set_margin_end(box, 12);
	gtk_widget_set_margin_top(box, 12);
	gtk_widget_set_margin_bottom(box, 12);

	g = group(box, "Card");
	p->v_dev = value_row(g, "Device");
	p->v_fw = value_row(g, "Firmware");
	p->v_img = value_row(g, "Image");
	p->v_state = value_row(g, "Decoder");
	p->v_engine = value_row(g, "Engine control");
	p->v_duty = value_row(g, "Busy (getPercent)");

	g = group(box, "Thermal");
	p->v_t_back = value_row(g, "Back card");
	p->v_t_die = value_row(g, "FPGA die");
	p->v_t_face = value_row(g, "Faceplate");
	p->v_t_bottom = value_row(g, "Bottom");

	g = group(box, "Clip");
	p->r_file = adw_action_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->r_file), "File");
	adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(p->r_file), FALSE);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(p->r_file), "—");
	adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(p->r_file), 3);
	adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(p->r_file), TRUE);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(g), p->r_file);
	p->v_codec = value_row(g, "Codec");
	p->v_size = value_row(g, "Size");
	p->v_fps = value_row(g, "Frame rate");
	p->v_frames = value_row(g, "Frames");
	p->v_colour = value_row(g, "Colour");
	p->v_scan = value_row(g, "Scan");
	p->v_buf = value_row(g, "Card buffers");

	g = group(box, "Session");
	p->v_decoded = value_row(g, "Decoded");
	p->v_dec_ms = value_row(g, "Decode last/avg/max");
	p->v_copy_ms = value_row(g, "File read / readback");
	p->v_upload_ms = value_row(g, "GPU upload");
	p->v_rates = value_row(g, "Frames per second");
	p->v_shown = value_row(g, "Presentation");
	p->v_ladder = value_row(g, "Recovery ladder");
	p->v_hist = value_row(g, "Decode histogram (ms)");
	p->v_gl = value_row(g, "Renderer");

	reset = gtk_button_new_with_mnemonic("_Reset Statistics");
	gtk_actionable_set_action_name(GTK_ACTIONABLE(reset), "win.reset-stats");
	gtk_widget_set_halign(reset, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(box), reset);

	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), box);
	return sw;
}

static GtkWidget *flat_button(const char *icon, const char *action, const char *tip)
{
	GtkWidget *b = gtk_button_new_from_icon_name(icon);

	gtk_widget_add_css_class(b, "flat");
	gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
	gtk_widget_set_tooltip_text(b, tip);
	gtk_widget_set_focusable(b, FALSE); /* keys belong to the player */
	gtk_widget_set_focus_on_click(b, FALSE);
	return b;
}

static GtkWidget *build_controls(Player *p)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *loop = gtk_toggle_button_new();

	gtk_widget_add_css_class(box, "toolbar");
	gtk_box_append(GTK_BOX(box), flat_button("media-seek-backward-symbolic", "win.step-back",
						 "Previous Frame (←)"));
	p->play_btn = flat_button("media-playback-start-symbolic", "win.play-pause", "Play (Space)");
	gtk_box_append(GTK_BOX(box), p->play_btn);
	gtk_box_append(GTK_BOX(box), flat_button("media-seek-forward-symbolic", "win.step-forward",
						 "Next Frame (→)"));

	p->time_lbl = gtk_label_new("00:00.000");
	gtk_widget_add_css_class(p->time_lbl, "numeric");
	gtk_box_append(GTK_BOX(box), p->time_lbl);

	p->scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 1);
	gtk_scale_set_draw_value(GTK_SCALE(p->scale), FALSE);
	gtk_widget_set_hexpand(p->scale, TRUE);
	gtk_widget_set_focusable(p->scale, FALSE);
	g_signal_connect(p->scale, "change-value", G_CALLBACK(on_scale_change), p);
	gtk_box_append(GTK_BOX(box), p->scale);

	p->dur_lbl = gtk_label_new("00:00.000");
	gtk_widget_add_css_class(p->dur_lbl, "numeric");
	gtk_widget_add_css_class(p->dur_lbl, "dim-label");
	gtk_box_append(GTK_BOX(box), p->dur_lbl);

	p->frame_lbl = gtk_label_new("0 / 0");
	gtk_widget_add_css_class(p->frame_lbl, "numeric");
	gtk_widget_add_css_class(p->frame_lbl, "ab-frame");
	gtk_widget_set_tooltip_text(p->frame_lbl, "Frame index / last frame");
	gtk_box_append(GTK_BOX(box), p->frame_lbl);

	gtk_button_set_icon_name(GTK_BUTTON(loop), "media-playlist-repeat-symbolic");
	gtk_widget_add_css_class(loop, "flat");
	gtk_actionable_set_action_name(GTK_ACTIONABLE(loop), "win.loop");
	gtk_widget_set_tooltip_text(loop, "Loop (L)");
	gtk_widget_set_focusable(loop, FALSE);
	gtk_box_append(GTK_BOX(box), loop);

	p->fs_btn = flat_button("view-fullscreen-symbolic", "win.fullscreen", "Fullscreen (F)");
	gtk_box_append(GTK_BOX(box), p->fs_btn);
	return box;
}

static GMenuModel *build_menu(void)
{
	GMenu *menu = g_menu_new(), *s, *colour = g_menu_new(), *card = g_menu_new(), *cs;
	static const char *const mats[][2] = {
		{ "_Automatic", "auto" }, { "BT.60_1", "601" }, { "BT._709", "709" }, { "BT.2_020", "2020" },
	};

	s = g_menu_new();
	g_menu_append(s, "_Open…", "win.open");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(s));
	g_object_unref(s);

	s = g_menu_new();
	g_menu_append(s, "_Loop", "win.loop");
	g_menu_append(s, "Run at _Card Speed", "win.card-speed");
	g_menu_append(s, "Statistics _Overlay", "win.osd");
	g_menu_append(s, "Card & Statistics _Panel", "win.sidebar");
	cs = g_menu_new();
	for (size_t i = 0; i < G_N_ELEMENTS(mats); i++) {
		GMenuItem *it = g_menu_item_new(mats[i][0], NULL);

		g_menu_item_set_action_and_target_value(it, "win.matrix", g_variant_new_string(mats[i][1]));
		g_menu_append_item(cs, it);
		g_object_unref(it);
	}
	g_menu_append_section(colour, NULL, G_MENU_MODEL(cs));
	g_object_unref(cs);
	cs = g_menu_new();
	g_menu_append(cs, "_Full-Range Levels", "win.full-range");
	g_menu_append_section(colour, NULL, G_MENU_MODEL(cs));
	g_object_unref(cs);
	g_menu_append_submenu(s, "Colour _Matrix", G_MENU_MODEL(colour));

	cs = g_menu_new();
	g_menu_append(cs, "_Start Decoder", "win.card-start");
	g_menu_append(cs, "S_top Decoder", "win.card-stop");
	g_menu_append(cs, "_Hard Reset (FLR)…", "win.card-reset");
	g_menu_append_section(card, NULL, G_MENU_MODEL(cs));
	g_object_unref(cs);
	cs = g_menu_new();
	g_menu_append(cs, "Use Si_mulated Card", "win.simulate");
	g_menu_append_section(card, NULL, G_MENU_MODEL(cs));
	g_object_unref(cs);
	g_menu_append_submenu(s, "C_ard", G_MENU_MODEL(card));
	g_menu_append_section(menu, NULL, G_MENU_MODEL(s));
	g_object_unref(s);

	s = g_menu_new();
	g_menu_append(s, "Save Frame as _PNG…", "win.save-png");
	g_menu_append(s, "Save _Raw Card Output…", "win.save-raw");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(s));
	g_object_unref(s);

	s = g_menu_new();
	g_menu_append(s, "_Reset Statistics", "win.reset-stats");
	g_menu_append(s, "_Keyboard Shortcuts", "win.shortcuts");
	g_menu_append(s, "_About Afterburner Player", "app.about");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(s));
	g_object_unref(s);
	g_object_unref(colour);
	g_object_unref(card);
	return G_MENU_MODEL(menu);
}

static void player_free(gpointer data)
{
	Player *p = data;

	g_free(p->path);
	g_free(p);
}

static const char css[] =
	".ab-osd { background-color: alpha(black, 0.62); color: white; border-radius: 9px;"
	"  padding: 8px 12px; font-family: monospace; font-size: 9pt; }"
	".ab-badge { background-color: #e5a50a; color: #1e1e1e; border-radius: 6px;"
	"  padding: 3px 9px; font-weight: 800; font-size: 9pt; }"
	".ab-video { background-color: black; }"
	".ab-frame { min-width: 9em; }";

static Player *build_window(GtkApplication *app)
{
	Player *p = g_new0(Player, 1);
	GtkWidget *header, *open_btn, *side_btn, *content, *box, *spin_box, *cancel;
	GtkEventController *key, *motion;
	GtkDropTarget *drop;
	GMenuModel *menu;
	AdwBreakpoint *bp;
	GValue t = G_VALUE_INIT;
	GtkCssProvider *prov;

	p->fd = -1;
	p->pres_gen = GEN_INVALID;
	p->window = adw_application_window_new(app);
	g_object_set_data_full(G_OBJECT(p->window), "player", p, player_free);
	gtk_window_set_title(GTK_WINDOW(p->window), APP_NAME);
	gtk_window_set_default_size(GTK_WINDOW(p->window), 1280, 800);
	gtk_widget_set_size_request(p->window, 360, 320);
	g_action_map_add_action_entries(G_ACTION_MAP(p->window), win_actions, G_N_ELEMENTS(win_actions), p);

	prov = gtk_css_provider_new();
	gtk_css_provider_load_from_string(prov, css);
	gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(prov),
						   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(prov);

	/* header */
	header = adw_header_bar_new();
	p->title = adw_window_title_new(APP_NAME, "");
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), p->title);
	open_btn = gtk_button_new_from_icon_name("document-open-symbolic");
	gtk_actionable_set_action_name(GTK_ACTIONABLE(open_btn), "win.open");
	gtk_widget_set_tooltip_text(open_btn, "Open (Ctrl+O)");
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), open_btn);
	menu = build_menu();
	p->menu_btn = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(p->menu_btn), "open-menu-symbolic");
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(p->menu_btn), menu);
	gtk_menu_button_set_primary(GTK_MENU_BUTTON(p->menu_btn), TRUE);
	gtk_widget_set_tooltip_text(p->menu_btn, "Main Menu");
	g_object_unref(menu);
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), p->menu_btn);
	side_btn = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(side_btn), "sidebar-show-right-symbolic");
	gtk_actionable_set_action_name(GTK_ACTIONABLE(side_btn), "win.sidebar");
	gtk_widget_set_tooltip_text(side_btn, "Card & Statistics (F9)");
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), side_btn);

	/* pages */
	p->stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(p->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);

	p->welcome = adw_status_page_new();
	adw_status_page_set_icon_name(ADW_STATUS_PAGE(p->welcome), "video-x-generic-symbolic");
	adw_status_page_set_title(ADW_STATUS_PAGE(p->welcome), APP_NAME);
	adw_status_page_set_description(ADW_STATUS_PAGE(p->welcome),
		"Decode ProRes 422 clips on the Apple Afterburner card.\n"
		"Drop a .mov file here or open one.");
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
	gtk_box_append(GTK_BOX(box), pill("_Open File…", "win.open", true));
	p->welcome_card = gtk_label_new("");
	gtk_widget_add_css_class(p->welcome_card, "dim-label");
	gtk_label_set_wrap(GTK_LABEL(p->welcome_card), TRUE);
	gtk_label_set_justify(GTK_LABEL(p->welcome_card), GTK_JUSTIFY_CENTER);
	gtk_box_append(GTK_BOX(box), p->welcome_card);
	adw_status_page_set_child(ADW_STATUS_PAGE(p->welcome), box);
	gtk_stack_add_named(GTK_STACK(p->stack), p->welcome, "welcome");

	p->load_page = adw_status_page_new();
	adw_status_page_set_title(ADW_STATUS_PAGE(p->load_page), "Checking Frames");
	spin_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	p->loading_bar = gtk_progress_bar_new();
	gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(p->loading_bar), TRUE);
	gtk_widget_set_size_request(p->loading_bar, 320, -1);
	gtk_widget_set_halign(p->loading_bar, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(spin_box), p->loading_bar);
	cancel = gtk_label_new("Every sample's offset, size and frame header is validated\n"
			       "before anything is sent to the card.");
	gtk_widget_add_css_class(cancel, "dim-label");
	gtk_label_set_justify(GTK_LABEL(cancel), GTK_JUSTIFY_CENTER);
	gtk_box_append(GTK_BOX(spin_box), cancel);
	adw_status_page_set_child(ADW_STATUS_PAGE(p->load_page), spin_box);
	gtk_stack_add_named(GTK_STACK(p->stack), p->load_page, "loading");

	p->error_page = adw_status_page_new();
	adw_status_page_set_icon_name(ADW_STATUS_PAGE(p->error_page), "dialog-warning-symbolic");
	p->error_buttons = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	adw_status_page_set_child(ADW_STATUS_PAGE(p->error_page), p->error_buttons);
	gtk_stack_add_named(GTK_STACK(p->stack), p->error_page, "error");

	p->view = video_view_new();
	gtk_widget_add_css_class(video_view_widget(p->view), "ab-video");
	g_signal_connect_after(video_view_widget(p->view), "realize", G_CALLBACK(on_view_realized), p);
	p->overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(p->overlay), video_view_widget(p->view));
	p->osd = gtk_label_new("");
	gtk_widget_add_css_class(p->osd, "ab-osd");
	gtk_widget_set_halign(p->osd, GTK_ALIGN_START);
	gtk_widget_set_valign(p->osd, GTK_ALIGN_START);
	gtk_widget_set_margin_start(p->osd, 12);
	gtk_widget_set_margin_top(p->osd, 12);
	gtk_widget_set_can_target(p->osd, FALSE);
	gtk_overlay_add_overlay(GTK_OVERLAY(p->overlay), p->osd);
	p->badge = gtk_label_new("SIMULATED CARD");
	gtk_widget_add_css_class(p->badge, "ab-badge");
	gtk_widget_set_halign(p->badge, GTK_ALIGN_END);
	gtk_widget_set_valign(p->badge, GTK_ALIGN_START);
	gtk_widget_set_margin_end(p->badge, 12);
	gtk_widget_set_margin_top(p->badge, 12);
	gtk_widget_set_can_target(p->badge, FALSE);
	gtk_widget_set_visible(p->badge, FALSE);
	gtk_overlay_add_overlay(GTK_OVERLAY(p->overlay), p->badge);
	gtk_stack_add_named(GTK_STACK(p->stack), p->overlay, "player");

	/* split view: video | card & statistics */
	p->split = adw_overlay_split_view_new();
	adw_overlay_split_view_set_sidebar_position(ADW_OVERLAY_SPLIT_VIEW(p->split), GTK_PACK_END);
	adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(p->split), FALSE);
	adw_overlay_split_view_set_min_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(p->split), 300);
	adw_overlay_split_view_set_max_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(p->split), 380);
	adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(p->split), p->stack);
	adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(p->split), build_sidebar(p));
	g_signal_connect(p->split, "notify::show-sidebar", G_CALLBACK(on_sidebar_shown), p);

	p->tview = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(p->tview), header);
	p->controls = build_controls(p);
	gtk_widget_set_visible(p->controls, FALSE);
	adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(p->tview), p->controls);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(p->tview), p->split);

	p->toasts = adw_toast_overlay_new();
	adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(p->toasts), p->tview);
	content = p->toasts;
	adw_application_window_set_content(ADW_APPLICATION_WINDOW(p->window), content);

	/* narrow windows: sidebar overlays the video instead of shrinking it */
	bp = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 760sp"));
	g_value_init(&t, G_TYPE_BOOLEAN);
	g_value_set_boolean(&t, TRUE);
	adw_breakpoint_add_setter(bp, G_OBJECT(p->split), "collapsed", &t);
	g_value_unset(&t);
	adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(p->window), bp);

	/* input */
	key = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(key, GTK_PHASE_CAPTURE);
	g_signal_connect(key, "key-pressed", G_CALLBACK(on_key), p);
	gtk_widget_add_controller(p->window, key);
	motion = gtk_event_controller_motion_new();
	g_signal_connect(motion, "motion", G_CALLBACK(on_motion), p);
	gtk_widget_add_controller(p->tview, motion);
	drop = gtk_drop_target_new(G_TYPE_FILE, GDK_ACTION_COPY);
	g_signal_connect(drop, "drop", G_CALLBACK(on_drop), p);
	gtk_widget_add_controller(p->tview, GTK_EVENT_CONTROLLER(drop));

	g_signal_connect(p->window, "close-request", G_CALLBACK(on_close_request), p);
	g_signal_connect(p->window, "notify::fullscreened", G_CALLBACK(on_fullscreen_changed), p);

	if (opt.simulate)
		set_action_state(p, "simulate", g_variant_new_boolean(TRUE));
	if (opt.loop)
		set_action_state(p, "loop", g_variant_new_boolean(TRUE));
	if (opt.card_speed)
		set_action_state(p, "card-speed", g_variant_new_boolean(TRUE));
	if (opt.export_png)
		set_action_state(p, "osd", g_variant_new_boolean(FALSE));
	if (opt.panel)
		g_action_group_change_action_state(G_ACTION_GROUP(p->window), "sidebar",
						   g_variant_new_boolean(TRUE));
	gtk_widget_set_visible(p->osd, !opt.export_png);
	close_clip(p); /* disables clip actions */
	show_page(p, "welcome");
	update_card_info(p);
	update_session_info(p);
	update_clip_info(p);
	p->stats_id = g_timeout_add(1000, on_stats, p);
	return p;
}

/* ---- application ----------------------------------------------------------------- */

static Player *app_player(GtkApplication *app)
{
	GtkWindow *w = gtk_application_get_active_window(app);

	return w ? player_of(w) : build_window(app);
}

static void on_activate(GtkApplication *app, gpointer data)
{
	Player *p = app_player(app);

	(void)data;
	if (opt.export_png) {
		g_printerr("--export-png needs a file argument\n");
		opt.exit_status = 2;
		g_application_quit(G_APPLICATION(app));
		return;
	}
	gtk_window_present(GTK_WINDOW(p->window));
}

static void on_open(GApplication *app, GFile **files, int n, const char *hint, gpointer data)
{
	Player *p = app_player(GTK_APPLICATION(app));

	(void)hint;
	(void)data;
	gtk_window_present(GTK_WINDOW(p->window));
	if (n > 0)
		open_file(p, files[0]);
}

static void act_about(GSimpleAction *a, GVariant *v, gpointer app)
{
	GtkWindow *w = gtk_application_get_active_window(app);
	AdwDialog *d = adw_about_dialog_new();

	(void)a;
	(void)v;
	adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(d), APP_NAME);
	adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(d), APP_ID);
	adw_about_dialog_set_version(ADW_ABOUT_DIALOG(d), APP_VERSION);
	adw_about_dialog_set_developer_name(ADW_ABOUT_DIALOG(d), "Afterburner-on-Linux contributors");
	adw_about_dialog_set_website(ADW_ABOUT_DIALOG(d), APP_URL);
	adw_about_dialog_set_issue_url(ADW_ABOUT_DIALOG(d), APP_URL "/issues");
	adw_about_dialog_set_license_type(ADW_ABOUT_DIALOG(d), GTK_LICENSE_GPL_2_0_ONLY);
	adw_about_dialog_set_comments(ADW_ABOUT_DIALOG(d),
		"Realtime ProRes 422 player and test tool for the Apple Afterburner card on Linux. "
		"Every frame is decoded by the card through /dev/afterburner0; this app parses the "
		"container, feeds packets and converts the card's UYVY output for display.");
	adw_dialog_present(d, w ? GTK_WIDGET(w) : NULL);
}

static void act_quit(GSimpleAction *a, GVariant *v, gpointer app)
{
	/* closing destroys windows, which edits the app's list: walk a copy */
	GList *wins = g_list_copy(gtk_application_get_windows(app));

	(void)a;
	(void)v;
	g_list_foreach(wins, (GFunc)(void (*)(void))g_object_ref, NULL);
	for (GList *l = wins; l; l = l->next)
		gtk_window_close(GTK_WINDOW(l->data));
	g_list_free_full(wins, g_object_unref);
}

static gint on_local_options(GApplication *app, GVariantDict *d, gpointer data)
{
	(void)d;
	(void)data;
	if (opt.export_png) /* scripted runs must not hand off to a running window */
		g_application_set_flags(app, g_application_get_flags(app) | G_APPLICATION_NON_UNIQUE);
	if (opt.frame < 0)
		opt.frame = 0;
	return -1;
}

int main(int argc, char **argv)
{
	static const GActionEntry app_actions[] = {
		{ "about", act_about, NULL, NULL, NULL, { 0 } },
		{ "quit", act_quit, NULL, NULL, NULL, { 0 } },
	};
	static const char *const accel_open[] = { "<Ctrl>o", NULL };
	static const char *const accel_png[] = { "<Ctrl>s", NULL };
	static const char *const accel_raw[] = { "<Ctrl><Shift>s", NULL };
	static const char *const accel_quit[] = { "<Ctrl>q", NULL };
	static const char *const accel_keys[] = { "<Ctrl>question", NULL };
	const GOptionEntry entries[] = {
		{ "simulate", 'S', 0, G_OPTION_ARG_NONE, &opt.simulate,
		  "Use the simulated card (no hardware)", NULL },
		{ "frame", 'n', 0, G_OPTION_ARG_INT64, &opt.frame, "Start at frame N", "N" },
		{ "paused", 'p', 0, G_OPTION_ARG_NONE, &opt.paused, "Start paused", NULL },
		{ "loop", 'l', 0, G_OPTION_ARG_NONE, &opt.loop, "Loop playback", NULL },
		{ "card-speed", 'b', 0, G_OPTION_ARG_NONE, &opt.card_speed,
		  "Decode as fast as the card allows (benchmark)", NULL },
		{ "panel", 0, 0, G_OPTION_ARG_NONE, &opt.panel,
		  "Start with the card & statistics panel open", NULL },
		{ "export-png", 0, 0, G_OPTION_ARG_FILENAME, &opt.export_png,
		  "Decode frame N (see --frame) on the card, save the displayed image as PNG, quit",
		  "PATH" },
		{ NULL, 0, 0, 0, NULL, NULL, NULL },
	};
	AdwApplication *app;
	int rc;

	g_set_application_name(APP_NAME);
	app = adw_application_new(APP_ID, G_APPLICATION_HANDLES_OPEN);
	g_application_add_main_option_entries(G_APPLICATION(app), entries);
	g_application_set_option_context_parameter_string(G_APPLICATION(app), "[FILE.mov]");
	g_application_set_option_context_summary(G_APPLICATION(app),
		"ProRes 422 player and test tool for the Apple Afterburner card.");
	g_action_map_add_action_entries(G_ACTION_MAP(app), app_actions, G_N_ELEMENTS(app_actions), app);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.open", accel_open);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.save-png", accel_png);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.save-raw", accel_raw);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.quit", accel_quit);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.shortcuts", accel_keys);
	g_signal_connect(app, "handle-local-options", G_CALLBACK(on_local_options), NULL);
	g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
	g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);
	gtk_window_set_default_icon_name(APP_ID);

	rc = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return rc ? rc : opt.exit_status;
}
