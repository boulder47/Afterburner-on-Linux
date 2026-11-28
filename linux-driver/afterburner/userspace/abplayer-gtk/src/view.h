/* SPDX-License-Identifier: GPL-2.0 */
/*
 * view.h — GtkGLArea that shows card output ([U,Y0,V,Y1] = UYVY) frames.
 *
 * Pass 1 converts UYVY -> RGB at native resolution into an FBO texture
 * (exactly one fragment per source pixel). Pass 2 draws that texture
 * aspect-fit with trilinear mipmapping, so 4K downscales cleanly.
 */
#ifndef AB_VIEW_H
#define AB_VIEW_H

#include <gtk/gtk.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct VideoView VideoView;

VideoView *video_view_new(void);              /* freed with its widget */
GtkWidget *video_view_widget(VideoView *v);

void video_view_set_format(VideoView *v, uint32_t width, uint32_t height,
			   uint32_t stride, double pixel_aspect);
/* matrix: 1 BT.709, 6 BT.601, 9 BT.2020 */
void video_view_set_colour(VideoView *v, int matrix, bool full_range);

/* Upload + convert one frame now (GL context made current). Returns false
 * if GL is not ready yet (widget unrealized) or unavailable. */
bool video_view_upload(VideoView *v, const uint8_t *uyvy, uint64_t *upload_ns);
void video_view_clear(VideoView *v);
bool video_view_has_frame(VideoView *v);

/* Native-resolution RGBA of the last converted frame — exactly what is on
 * screen before scaling. NULL if no frame. */
GdkTexture *video_view_read_texture(VideoView *v);

const char *video_view_error(VideoView *v);   /* NULL if GL is fine */
const char *video_view_renderer(VideoView *v); /* "Mesa Intel(R) ... (OpenGL 4.6)" */

#endif
