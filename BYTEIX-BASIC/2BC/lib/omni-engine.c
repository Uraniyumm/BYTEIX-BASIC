#include "omni-engine.h"


void omni_init(int width, int height, const char *title) {
    InitWindow(width, height, title);
}
void omni_begin_drawing() {
    BeginDrawing();
}
void omni_end_drawing() {
    EndDrawing();
}
void window_should_close(void) {
    CloseWindow();
}
void omni_draw_text(const char *text, int posX, int posY, int fontSize, Color color) {
    DrawText(text, posX, posY, fontSize, color);
}
void omni_draw_rectangle(int posX, int posY, int width, int height, Color color) {
    DrawRectangle(posX, posY, width, height, color);
}
void omni_draw_circle(int centerX, int centerY, float radius, Color color) {
    DrawCircle(centerX, centerY, radius, color);
}
void omni_draw_triangle(int x1, int y1, int x2, int y2, int x3, int y3, Color color) {
    DrawTriangle((Vector2){x1, y1}, (Vector2){x2, y2}, (Vector2){x3, y3}, color);
}
void omni_image(const char *filename, int posX, int posY, float scaleX, float scaleY) {
    DrawTextureEx(LoadTexture(filename), (Vector2){posX, posY}, 0.0f, scaleX, WHITE);
}