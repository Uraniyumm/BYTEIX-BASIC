#ifndef OMNI_ENGINE_H
#define OMNI_ENGINE_H

// The Omni-Engine is the libraries for developing games in BYTEIX-BASIC. 
// It uses wrappers for the raylib library to give its own syntax.
#include <raylib.h>

void omni_init(int width, int height, const char *title);
void omni_begin_drawing();
void omni_end_drawing();
void window_should_close(void);

int window_true(void);
void omni_draw_text(const char *text, int posX, int posY, int fontSize, Color color);
void omni_draw_rectangle(int posX, int posY, int width, int height, Color color);
void omni_draw_circle(int centerX, int centerY, float radius, Color color);
void omni_draw_triangle(int x1, int y1, int x2, int y2, int x3, int y3, Color color);
void omni_image(const char *filename, int posX, int posY, float scaleX, float scaleY);

#endif // OMNI_ENGINE_H