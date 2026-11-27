// SPDX-License-Identifier: GPL-2.0
/*
 * view.c — GL presentation of card output frames (see view.h).
 *
 * The card's fixed output (FINDINGS §35) is interleaved 8-bit 4:2:2 with
 * byte order [U, Y0, V, Y1] — i.e. UYVY. It is uploaded unconverted as an
 * RGBA8 texture of ceil(W/2) x H texels (R=U, G=Y0, B=V, A=Y1) using
 * GL_UNPACK_ROW_LENGTH for the card's 64-byte padded stride.
 */
#include "view.h"

#include <epoxy/gl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

struct VideoView {
	GtkWidget *area;
	char *error;
	char renderer[256];
	bool es;

	GLuint vao, vbo, prog_conv, prog_blit;
	GLint u_src, u_m, u_off, u_gw, u_tex;
	GLuint tex_src, tex_rgb, fbo;
	uint32_t tex_w, tex_h;      /* allocated texture size */

	uint32_t w, h, stride;
	double par;
	int matrix;
	bool full_range;
	bool have_frame;
	int fb_w, fb_h;
};

static const char *vs_src =
	"in vec2 a_pos;\n"
	"in vec2 a_uv;\n"
	"out vec2 v_uv;\n"
	"void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

/* one fragment per output pixel; FBO row y == image row y (top first) */
static const char *fs_conv_src =
	"uniform highp sampler2D u_src;\n"
	"uniform mat3 u_m;\n"
	"uniform vec3 u_off;\n"
	"uniform int u_gw;\n"
	"in vec2 v_uv;\n"
	"out vec4 o_color;\n"
	"void main() {\n"
	"  ivec2 p = ivec2(gl_FragCoord.xy);\n"
	"  int gx = p.x / 2;\n"
	"  vec4 g = texelFetch(u_src, ivec2(gx, p.y), 0);\n"
	"  float y;\n"
	"  vec2 c;\n"
	"  if ((p.x & 1) == 0) {\n"
	"    y = g.g; c = g.rb;\n"             /* chroma co-sited with even luma */
	"  } else {\n"
	"    vec4 n = texelFetch(u_src, ivec2(min(gx + 1, u_gw - 1), p.y), 0);\n"
	"    y = g.a; c = 0.5 * (g.rb + n.rb);\n"
	"  }\n"
	"  vec3 rgb = u_m * (vec3(y, c) * 255.0 - u_off);\n"
	"  o_color = vec4(clamp(rgb, 0.0, 1.0), 1.0);\n"
	"}\n";

static const char *fs_blit_src =
	"uniform sampler2D u_tex;\n"
	"in vec2 v_uv;\n"
	"out vec4 o_color;\n"
	"void main() { o_color = texture(u_tex, v_uv); }\n";

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void set_error(VideoView *v, const char *msg)
{
	g_free(v->error);
	v->error = g_strdup(msg);
	g_warning("video view: %s", msg);
}

static GLuint compile(VideoView *v, GLenum type, const char *body)
{
	const char *hdr = v->es ? "#version 300 es\nprecision highp float;\nprecision highp int;\n"
				: "#version 150\n";
	const char *srcs[2] = { hdr, body };
	GLuint s = glCreateShader(type);
	GLint ok = 0;

	glShaderSource(s, 2, srcs, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024] = "";

		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		set_error(v, log);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

static GLuint link_prog(VideoView *v, const char *fs_body)
{
	GLuint vs = compile(v, GL_VERTEX_SHADER, vs_src);
	GLuint fs = compile(v, GL_FRAGMENT_SHADER, fs_body);
	GLuint p;
	GLint ok = 0;

	if (!vs || !fs)
		return 0;
	p = glCreateProgram();
	glAttachShader(p, vs);
	glAttachShader(p, fs);
	glBindAttribLocation(p, 0, "a_pos");
	glBindAttribLocation(p, 1, "a_uv");
	glLinkProgram(p);
	glDeleteShader(vs);
	glDeleteShader(fs);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[1024] = "";

		glGetProgramInfoLog(p, sizeof(log), NULL, log);
		set_error(v, log);
		glDeleteProgram(p);
		return 0;
	}
	return p;
}

static void on_realize(GtkGLArea *area, VideoView *v)
{
	/* x, y, u, v — screen top-left samples texture row 0 (= image top) */
	static const GLfloat quad[] = {
		-1, 1, 0, 0,  -1, -1, 0, 1,  1, 1, 1, 0,  1, -1, 1, 1,
	};
	GdkGLContext *ctx;
	int maj = 0, min = 0;

	gtk_gl_area_make_current(area);
	if (gtk_gl_area_get_error(area)) {
		set_error(v, gtk_gl_area_get_error(area)->message);
		return;
	}
	ctx = gtk_gl_area_get_context(area);
	v->es = gdk_gl_context_get_use_es(ctx);
	gdk_gl_context_get_version(ctx, &maj, &min);
	snprintf(v->renderer, sizeof(v->renderer), "%s (%s %d.%d)",
		 (const char *)glGetString(GL_RENDERER), v->es ? "OpenGL ES" : "OpenGL", maj, min);

	v->prog_conv = link_prog(v, fs_conv_src);
	v->prog_blit = link_prog(v, fs_blit_src);
	if (!v->prog_conv || !v->prog_blit)
		return;
	v->u_src = glGetUniformLocation(v->prog_conv, "u_src");
	v->u_m = glGetUniformLocation(v->prog_conv, "u_m");
	v->u_off = glGetUniformLocation(v->prog_conv, "u_off");
	v->u_gw = glGetUniformLocation(v->prog_conv, "u_gw");
	v->u_tex = glGetUniformLocation(v->prog_blit, "u_tex");

	glGenVertexArrays(1, &v->vao);
	glBindVertexArray(v->vao);
	glGenBuffers(1, &v->vbo);
	glBindBuffer(GL_ARRAY_BUFFER, v->vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
			      (void *)(2 * sizeof(GLfloat)));
	glBindVertexArray(0);
	g_clear_pointer(&v->error, g_free);
}

static void free_textures(VideoView *v)
{
	if (v->fbo)
		glDeleteFramebuffers(1, &v->fbo);
	if (v->tex_src)
		glDeleteTextures(1, &v->tex_src);
	if (v->tex_rgb)
		glDeleteTextures(1, &v->tex_rgb);
	v->fbo = v->tex_src = v->tex_rgb = 0;
	v->tex_w = v->tex_h = 0;
}

static void on_unrealize(GtkGLArea *area, VideoView *v)
{
	gtk_gl_area_make_current(area);
	if (gtk_gl_area_get_error(area))
		return;
	free_textures(v);
	if (v->vbo)
		glDeleteBuffers(1, &v->vbo);
	if (v->vao)
		glDeleteVertexArrays(1, &v->vao);
	if (v->prog_conv)
		glDeleteProgram(v->prog_conv);
	if (v->prog_blit)
		glDeleteProgram(v->prog_blit);
	v->vbo = v->vao = v->prog_conv = v->prog_blit = 0;
	v->have_frame = false;
}

static void on_resize(GtkGLArea *area, int w, int h, VideoView *v)
{
	(void)area;
	v->fb_w = w;
	v->fb_h = h;
}

static bool ensure_textures(VideoView *v)
{
	uint32_t gw = (v->w + 1) / 2;

	if (v->tex_src && v->tex_w == v->w && v->tex_h == v->h)
		return true;
	free_textures(v);

	glGenTextures(1, &v->tex_src);
	glBindTexture(GL_TEXTURE_2D, v->tex_src);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)gw, (GLsizei)v->h, 0, GL_RGBA,
		     GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glGenTextures(1, &v->tex_rgb);
	glBindTexture(GL_TEXTURE_2D, v->tex_rgb);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)v->w, (GLsizei)v->h, 0, GL_RGBA,
		     GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenerateMipmap(GL_TEXTURE_2D); /* allocate the chain */

	glGenFramebuffers(1, &v->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, v->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, v->tex_rgb, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		set_error(v, "RGBA8 framebuffer incomplete");
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		free_textures(v);
		return false;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	v->tex_w = v->w;
	v->tex_h = v->h;
	return true;
}

/* rgb = M * (yuv8 - off); M column-major, folds range scaling in */
static void colour_matrix(int matrix, bool full, GLfloat m[9], GLfloat off[3])
{
	double kr = 0.2126, kb = 0.0722, kg, ys, cs;

	if (matrix == 6) { kr = 0.299; kb = 0.114; }
	else if (matrix == 9) { kr = 0.2627; kb = 0.0593; }
	kg = 1.0 - kr - kb;
	ys = full ? 1.0 / 255.0 : 1.0 / 219.0;
	cs = full ? 1.0 / 255.0 : 1.0 / 224.0;
	off[0] = full ? 0.0f : 16.0f;
	off[1] = off[2] = 128.0f;
	/* column 0: Y */
	m[0] = (GLfloat)ys; m[1] = (GLfloat)ys; m[2] = (GLfloat)ys;
	/* column 1: Cb */
	m[3] = 0.0f;
	m[4] = (GLfloat)(-2.0 * kb * (1.0 - kb) / kg * cs);
	m[5] = (GLfloat)((2.0 - 2.0 * kb) * cs);
	/* column 2: Cr */
	m[6] = (GLfloat)((2.0 - 2.0 * kr) * cs);
	m[7] = (GLfloat)(-2.0 * kr * (1.0 - kr) / kg * cs);
	m[8] = 0.0f;
}

static void convert(VideoView *v)
{
	GLfloat m[9], off[3];

	colour_matrix(v->matrix, v->full_range, m, off);
	glBindFramebuffer(GL_FRAMEBUFFER, v->fbo);
	glViewport(0, 0, (GLsizei)v->w, (GLsizei)v->h);
	glDisable(GL_BLEND);
	glUseProgram(v->prog_conv);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, v->tex_src);
	glUniform1i(v->u_src, 0);
	glUniformMatrix3fv(v->u_m, 1, GL_FALSE, m);
	glUniform3fv(v->u_off, 1, off);
	glUniform1i(v->u_gw, (GLint)((v->w + 1) / 2));
	glBindVertexArray(v->vao);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindVertexArray(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, v->tex_rgb);
	glGenerateMipmap(GL_TEXTURE_2D);
}

static gboolean on_render(GtkGLArea *area, GdkGLContext *ctx, VideoView *v)
{
	double dar, fb_ar;
	int vw, vh, vx, vy;

	(void)ctx;
	(void)area;
	glViewport(0, 0, v->fb_w, v->fb_h);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	if (!v->have_frame || v->error || !v->fb_w || !v->fb_h)
		return TRUE;

	dar = (double)v->w * v->par / (double)v->h;
	fb_ar = (double)v->fb_w / (double)v->fb_h;
	if (fb_ar > dar) {
		vh = v->fb_h;
		vw = (int)(vh * dar + 0.5);
	} else {
		vw = v->fb_w;
		vh = (int)(vw / dar + 0.5);
	}
	vx = (v->fb_w - vw) / 2;
	vy = (v->fb_h - vh) / 2;

	glViewport(vx, vy, vw, vh);
	glUseProgram(v->prog_blit);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, v->tex_rgb);
	glUniform1i(v->u_tex, 0);
	glBindVertexArray(v->vao);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindVertexArray(0);
	return TRUE;
}

static void view_destroy(gpointer data)
{
	VideoView *v = data;

	g_free(v->error);
	g_free(v);
}

VideoView *video_view_new(void)
{
	VideoView *v = g_new0(VideoView, 1);

	v->area = gtk_gl_area_new();
	v->par = 1.0;
	v->matrix = 1;
	gtk_gl_area_set_auto_render(GTK_GL_AREA(v->area), FALSE);
	gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(v->area), FALSE);
	gtk_widget_set_hexpand(v->area, TRUE);
	gtk_widget_set_vexpand(v->area, TRUE);
	gtk_widget_set_focusable(v->area, TRUE);
	g_signal_connect(v->area, "realize", G_CALLBACK(on_realize), v);
	g_signal_connect(v->area, "unrealize", G_CALLBACK(on_unrealize), v);
	g_signal_connect(v->area, "resize", G_CALLBACK(on_resize), v);
	g_signal_connect(v->area, "render", G_CALLBACK(on_render), v);
	g_object_set_data_full(G_OBJECT(v->area), "ab-video-view", v, view_destroy);
	return v;
}

GtkWidget *video_view_widget(VideoView *v) { return v->area; }

void video_view_set_format(VideoView *v, uint32_t width, uint32_t height, uint32_t stride,
			   double pixel_aspect)
{
	v->w = width;
	v->h = height;
	v->stride = stride;
	v->par = pixel_aspect > 0.01 ? pixel_aspect : 1.0;
	v->have_frame = false;
	gtk_gl_area_queue_render(GTK_GL_AREA(v->area));
}

void video_view_set_colour(VideoView *v, int matrix, bool full_range)
{
	v->matrix = matrix;
	v->full_range = full_range;
	if (v->have_frame && gtk_widget_get_realized(v->area) && !v->error) {
		gtk_gl_area_make_current(GTK_GL_AREA(v->area));
		convert(v); /* source texture is still resident */
		gtk_gl_area_queue_render(GTK_GL_AREA(v->area));
	}
}

bool video_view_upload(VideoView *v, const uint8_t *uyvy, uint64_t *upload_ns)
{
	uint64_t t0 = now_ns();

	if (!gtk_widget_get_realized(v->area) || v->error || !v->prog_conv || !v->w || !v->h)
		return false;
	gtk_gl_area_make_current(GTK_GL_AREA(v->area));
	if (gtk_gl_area_get_error(GTK_GL_AREA(v->area)) || !ensure_textures(v))
		return false;

	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(v->stride / 4));
	glBindTexture(GL_TEXTURE_2D, v->tex_src);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)((v->w + 1) / 2), (GLsizei)v->h,
			GL_RGBA, GL_UNSIGNED_BYTE, uyvy);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	convert(v);
	v->have_frame = true;
	gtk_gl_area_queue_render(GTK_GL_AREA(v->area));
	if (upload_ns)
		*upload_ns = now_ns() - t0;
	return true;
}

void video_view_clear(VideoView *v)
{
	v->have_frame = false;
	gtk_gl_area_queue_render(GTK_GL_AREA(v->area));
}

bool video_view_has_frame(VideoView *v) { return v->have_frame; }

GdkTexture *video_view_read_texture(VideoView *v)
{
	GBytes *bytes;
	guint8 *buf;
	GdkTexture *tex;
	size_t len;

	if (!v->have_frame || !gtk_widget_get_realized(v->area) || v->error)
		return NULL;
	gtk_gl_area_make_current(GTK_GL_AREA(v->area));
	len = (size_t)v->w * v->h * 4;
	buf = g_malloc(len);
	glBindFramebuffer(GL_FRAMEBUFFER, v->fbo);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glReadPixels(0, 0, (GLsizei)v->w, (GLsizei)v->h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	bytes = g_bytes_new_take(buf, len);
	tex = gdk_memory_texture_new((int)v->w, (int)v->h, GDK_MEMORY_R8G8B8A8, bytes,
				     (gsize)v->w * 4);
	g_bytes_unref(bytes);
	return tex;
}

const char *video_view_error(VideoView *v) { return v->error; }
const char *video_view_renderer(VideoView *v) { return v->renderer[0] ? v->renderer : "—"; }
