#ifndef GFX_RENDERER_H
#define GFX_RENDERER_H

/**
 * @brief Estrutura para representar coordenadas e dimensões em pixels.
 */
typedef struct {
    int x;
    int y;
} Point;

// Funções de inicialização
void gfx_renderer_init(void);

// Função principal de renderização - Deve ser chamada quando a tela estiver ativa
// Ela deve registrar o estado gráfico que precisa ser desenhado.
void gfx_render_start_frame(void);

/**
 * @brief Desenhamos um pixel individual em coordenadas (x, y).
 */
void gfx_draw_pixel(int x, int y, uint32_t color);

/**
 * @brief Desena um retângulo preenchido.
 * @param x Coordenada X superior esquerda.
 * @param y Coordenada Y superior esquerda.
 * @param w Largura do retângulo.
 * @param h Altura do retângulo.
 * @param color Cor de preenchimento (ARGB).
 */
void gfx_draw_rectangle(int x, int y, int w, int h, uint32_t color);

/**
 * @brief Desenhamos um texto em coordenadas específicas.
 * @param text O ponteiro para a string a ser desenhada.
 * @param x Coordenada X inicial.
 * @param y Coordenada Y vertical.
 * @param font Tamanho/ID da fonte (ex: 1, 2).
 */
void gfx_draw_text(const char *text, int x, int y, uint8_t font);

/**
 * @brief Finaliza o quadro de renderização e força o flush dos dados para a tela.
 */
void gfx_render_end_frame(void);

#endif /* GFX_RENDERER_H */