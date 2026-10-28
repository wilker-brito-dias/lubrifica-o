/*
 * Lubrifica-o — Firmware ESP32 para a bomba HF-28
 * Placa: LILYGO TTGO T-Display clássica (ESP32 + TFT ST7789)
 * Arduino-ESP32: série 3.x
 *
 * BLE:
 *   Nome    : Bomba_HF28
 *   Serviço : 0000aaaa-0000-1000-8000-00805f9b34fb
 *   Char.   : 0000bbbb-0000-1000-8000-00805f9b34fb (READ + WRITE protegidos)
 *   PIN     : 123456 (pareamento com Secure Connections + MITM + bonding)
 *
 * Relé:
 *   GPIO 26 — HIGH = bomba ligada; LOW = bomba desligada.
 *
 * Display LILYGO TTGO T-Display:
 *   TFT colorido ST7789, 240x135, SPI: MOSI=19, SCLK=18, CS=5, DC=16, RST=23, BL=4.
 *
 * IMPORTANTE:
 * - O relé começa em LOW e também é desligado em desconexão BLE.
 * - O display é acessório; o servidor BLE inicia antes da tela.
 * - Botão BOOT (GPIO0): animação visual de raiva por alguns segundos.
 * - Botão GPIO35: animação visual de dança por alguns segundos.
 *   Botões são apenas interações com o personagem e nunca acionam o relé.
 * - Para uma instalação industrial, use intertravamento/parada de emergência
 *   independente do ESP32 e driver apropriado para a bobina/carga do relé.
 */

#include <Arduino.h>
#include <esp_arduino_version.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLESecurity.h>

#if defined(CONFIG_BLUEDROID_ENABLED)
  #include <esp_gap_ble_api.h>
#endif

// --------------------------- Hardware ---------------------------------------

static constexpr uint8_t RELAY_PIN = 26;

// Pinagem interna da LILYGO TTGO T-Display clássica (TFT ST7789).
static constexpr uint8_t TFT_MOSI_PIN = 19;
static constexpr uint8_t TFT_SCLK_PIN = 18;
static constexpr uint8_t TFT_CS_PIN = 5;
static constexpr uint8_t TFT_DC_PIN = 16;
static constexpr uint8_t TFT_RST_PIN = 23;
static constexpr uint8_t TFT_BL_PIN = 4;
static constexpr uint16_t TFT_NATIVE_WIDTH = 135;
static constexpr uint16_t TFT_NATIVE_HEIGHT = 240;

static constexpr uint32_t BLE_STATIC_PIN = 123456;

// Botões internos da TTGO T-Display: BOOT=GPIO0 e botão de usuário=GPIO35.
// Ambos são ativos em LOW. GPIO35 é input-only e usa o pull-up da própria placa.
static constexpr uint8_t BUTTON_BOOT_PIN = 0;
static constexpr uint8_t BUTTON_DANCE_PIN = 35;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 35;
static constexpr uint32_t ANGRY_DURATION_MS = 3800;
static constexpr uint32_t DANCE_DURATION_MS = 7000;

Adafruit_ST7789 tft(&SPI, TFT_CS_PIN, TFT_DC_PIN, TFT_RST_PIN);
static bool tftReady = false;

// Buffer fora da tela: o rosto é composto em RAM e enviado em um único bloco SPI,
// evitando o flash momentâneo que ocorre ao apagar e redesenhar cada olho no TFT.
static constexpr int16_t FACE_AREA_X = 29;
static constexpr int16_t FACE_AREA_Y = 27;
static constexpr int16_t FACE_AREA_W = 182;
static constexpr int16_t FACE_AREA_H = 59;
GFXcanvas16 faceCanvas(FACE_AREA_W, FACE_AREA_H);

// --------------------------- BLE identifiers -------------------------------

static const char* BLE_DEVICE_NAME = "Bomba_HF28";
static const char* SERVICE_UUID = "0000aaaa-0000-1000-8000-00805f9b34fb";
static const char* CHARACTERISTIC_UUID = "0000bbbb-0000-1000-8000-00805f9b34fb";

static BLEServer* bleServer = nullptr;
static BLECharacteristic* commandCharacteristic = nullptr;

// Estas variáveis são lidas/escritas por tarefas diferentes (loop e callbacks BLE).
static volatile bool bleConnected = false;
static volatile bool relayActive = false;

// --------------------------- OLED / expressões ------------------------------

enum UiState : uint8_t {
  UI_BOOT = 0,
  UI_WAITING,
  UI_CONNECTED,
  UI_PUMP_ON,
  UI_PUMP_OFF,
  UI_DISCONNECTED
};

enum RobotInteraction : uint8_t {
  ROBOT_NORMAL = 0,
  ROBOT_ANGRY,
  ROBOT_DANCE
};

static RobotInteraction robotInteraction = ROBOT_NORMAL;
static uint32_t interactionStartedAt = 0;

struct ButtonDebouncer {
  uint8_t pin;
  bool rawState;
  bool stableState;
  uint32_t lastChangeAt;
  bool initialized;
};

static ButtonDebouncer bootButton{BUTTON_BOOT_PIN, true, true, 0, false};
static ButtonDebouncer danceButton{BUTTON_DANCE_PIN, true, true, 0, false};

// Leitura sem delay: não bloqueia BLE nem o controle do relé.
static bool buttonPressedEdge(ButtonDebouncer& button, uint32_t now) {
  const bool raw = digitalRead(button.pin);
  if (!button.initialized) {
    button.rawState = raw;
    button.stableState = raw;
    button.lastChangeAt = now;
    button.initialized = true;
    return false;
  }

  if (raw != button.rawState) {
    button.rawState = raw;
    button.lastChangeAt = now;
  }

  if ((now - button.lastChangeAt) >= BUTTON_DEBOUNCE_MS &&
      button.stableState != button.rawState) {
    button.stableState = button.rawState;
    return button.stableState == LOW; // dispara somente no pressionamento
  }

  return false;
}

static void initRobotButtons() {
  pinMode(BUTTON_BOOT_PIN, INPUT_PULLUP);
  // GPIO35 é somente entrada e não possui pull-up interno utilizável nesta placa.
  pinMode(BUTTON_DANCE_PIN, INPUT);
  const uint32_t now = millis();
  bootButton = {BUTTON_BOOT_PIN, static_cast<bool>(digitalRead(BUTTON_BOOT_PIN)),
                static_cast<bool>(digitalRead(BUTTON_BOOT_PIN)), now, true};
  danceButton = {BUTTON_DANCE_PIN, static_cast<bool>(digitalRead(BUTTON_DANCE_PIN)),
                 static_cast<bool>(digitalRead(BUTTON_DANCE_PIN)), now, true};
}

static void updateRobotInteractions(uint32_t now) {
  // Interações visuais apenas: essas funções não tocam no relé nem no BLE.
  if (buttonPressedEdge(bootButton, now)) {
    robotInteraction = ROBOT_ANGRY;
    interactionStartedAt = now;
    Serial.println("[UI] Botao BOOT: robo bravo.");
  } else if (buttonPressedEdge(danceButton, now)) {
    robotInteraction = ROBOT_DANCE;
    interactionStartedAt = now;
    Serial.println("[UI] Botao GPIO35: robo dancando.");
  }

  const uint32_t duration =
      (robotInteraction == ROBOT_ANGRY) ? ANGRY_DURATION_MS : DANCE_DURATION_MS;
  if (robotInteraction != ROBOT_NORMAL &&
      (now - interactionStartedAt) >= duration) {
    robotInteraction = ROBOT_NORMAL;
  }
}

struct UiSnapshot {
  UiState state;
  uint32_t changedAt;
};

static portMUX_TYPE uiMux = portMUX_INITIALIZER_UNLOCKED;
static volatile UiState currentUiState = UI_BOOT;
static volatile uint32_t uiChangedAt = 0;

static void setUiState(UiState newState) {
  portENTER_CRITICAL(&uiMux);
  currentUiState = newState;
  uiChangedAt = millis();
  portEXIT_CRITICAL(&uiMux);
}

static UiSnapshot getUiSnapshot() {
  UiSnapshot snapshot;
  portENTER_CRITICAL(&uiMux);
  snapshot.state = currentUiState;
  snapshot.changedAt = uiChangedAt;
  portEXIT_CRITICAL(&uiMux);
  return snapshot;
}

static const char* uiLabel(UiState state) {
  // Texto ASCII para a fonte padrão do Adafruit_GFX (não possui acentos).
  switch (state) {
    case UI_BOOT:         return "Iniciando Sistema";
    case UI_WAITING:      return "Aguardando Bluetooth";
    case UI_CONNECTED:    return "Conexao Estabelecida!";
    case UI_PUMP_ON:      return "BOMBA LIBERADA";
    case UI_PUMP_OFF:     return "Bomba Travada";
    case UI_DISCONNECTED: return "Conexao Perdida";
    default:              return "Bomba HF28";
  }
}

// Paleta fixa para manter a interface consistente e legível.
static constexpr uint16_t BG_COLOR = 0x0000;          // preto
static constexpr uint16_t FACE_COLOR = 0x0861;         // azul-marinho
static constexpr uint16_t PANEL_COLOR = 0x10A3;        // painel azul escuro
static constexpr uint16_t MUTED_COLOR = 0x7D9F;        // cinza azulado
static constexpr uint16_t CYAN_COLOR = 0x07FF;
static constexpr uint16_t GREEN_COLOR = 0x07E8;
static constexpr uint16_t RED_COLOR = 0xF986;
static constexpr uint16_t AMBER_COLOR = 0xFD20;

static uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
  return tft.color565(r, g, b);
}

static uint16_t statusColor(UiState state) {
  switch (state) {
    case UI_BOOT:         return AMBER_COLOR;
    case UI_WAITING:      return CYAN_COLOR;
    case UI_CONNECTED:    return GREEN_COLOR;
    case UI_PUMP_ON:      return GREEN_COLOR;
    case UI_PUMP_OFF:     return MUTED_COLOR;
    case UI_DISCONNECTED: return RED_COLOR;
    default:              return ST77XX_WHITE;
  }
}

static void drawCenteredStatusText(const char* text, int16_t y, uint16_t color) {
  tft.setTextWrap(false);

  int16_t x1 = 0, y1 = 0;
  uint16_t w = 0, h = 0;
  uint8_t size = 2;
  tft.setTextSize(size);
  tft.getTextBounds(text, 0, y, &x1, &y1, &w, &h);

  // Evita cortar os textos longos como "AGUARDANDO BLUETOOTH".
  if (w > 190) {
    size = 1;
    tft.setTextSize(size);
    tft.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  }

  int16_t x = (static_cast<int16_t>(tft.width()) - static_cast<int16_t>(w)) / 2;
  if (x < 0) x = 0;
  tft.setTextColor(color, PANEL_COLOR);
  tft.setCursor(x, y);
  tft.print(text);
}

// Desenha apenas a área do rosto. Não limpa nem redesenha o ecrã inteiro.
static void drawFaceExpression(UiState state, RobotInteraction interaction,
                               uint32_t elapsedMs, uint32_t nowMs) {
  if (!tftReady || faceCanvas.getBuffer() == nullptr) return;

  const uint16_t accent = statusColor(state);
  const bool waitingBlink = (nowMs % 4700U) < 150U;
  const bool normalBlink = (nowMs % 3900U) < 150U;

  // Coordenadas locais do canvas (0,0 corresponde ao pixel TFT 29,27).
  const int16_t leftEye = 51;
  const int16_t rightEye = 131;
  const int16_t eyeY = 24;

  // A composição ocorre toda em RAM; o TFT não é apagado durante o desenho.
  faceCanvas.fillScreen(FACE_COLOR);
  faceCanvas.drawPixel(14, 16, accent);
  faceCanvas.drawPixel(168, 16, accent);

  if (interaction == ROBOT_ANGRY) {
    // BRAVO: sobrancelhas inclinadas para dentro, olhos estreitos vermelhos,
    // pupilas firmes, dentes serrilhados e vapor que pulsa nas laterais.
    const uint8_t phase = static_cast<uint8_t>((nowMs / 115U) % 6U);
    const int16_t shake = (phase == 1 || phase == 4) ? 2 : 0;
    const uint16_t hot = (phase % 2U) ? color565(255, 55, 36) : RED_COLOR;

    faceCanvas.fillTriangle(24 + shake, 6, 71, 15, 68, 21, hot);
    faceCanvas.fillTriangle(158 - shake, 6, 111, 15, 114, 21, hot);

    faceCanvas.fillRoundRect(leftEye - 21 + shake, 17, 42, 19, 6, hot);
    faceCanvas.fillRoundRect(rightEye - 21 - shake, 17, 42, 19, 6, hot);
    faceCanvas.fillRoundRect(leftEye - 15 + shake, 21, 30, 10, 4, BG_COLOR);
    faceCanvas.fillRoundRect(rightEye - 15 - shake, 21, 30, 10, 4, BG_COLOR);
    faceCanvas.fillCircle(leftEye + 4 + shake, 25, 5, color565(255, 185, 70));
    faceCanvas.fillCircle(rightEye - 4 - shake, 25, 5, color565(255, 185, 70));
    faceCanvas.fillCircle(leftEye + 4 + shake, 25, 2, BG_COLOR);
    faceCanvas.fillCircle(rightEye - 4 - shake, 25, 2, BG_COLOR);

    // Boca irritada com dentes alternando entre cerrados e rosnado.
    if ((phase % 3U) == 0U) {
      faceCanvas.fillRoundRect(75, 43, 32, 11, 3, hot);
      faceCanvas.fillTriangle(78, 43, 85, 43, 82, 48, ST77XX_WHITE);
      faceCanvas.fillTriangle(88, 43, 95, 43, 91, 48, ST77XX_WHITE);
      faceCanvas.fillTriangle(99, 43, 105, 43, 102, 48, ST77XX_WHITE);
    } else {
      faceCanvas.drawLine(77, 46, 86, 50, hot);
      faceCanvas.drawLine(86, 50, 96, 47, hot);
      faceCanvas.drawLine(96, 47, 105, 50, hot);
    }

    // Marcas de raiva e vapor em movimento; sem escrever diretamente em pixels soltos no TFT.
    faceCanvas.drawLine(18, 22, 25, 17, hot);
    faceCanvas.drawLine(20, 27, 27, 23, hot);
    faceCanvas.drawLine(157, 17, 164, 22, hot);
    faceCanvas.drawLine(155, 23, 162, 27, hot);
    if ((phase % 2U) == 0U) {
      faceCanvas.drawCircle(12, 14, 4, AMBER_COLOR);
      faceCanvas.drawCircle(169, 14, 4, AMBER_COLOR);
      faceCanvas.drawPixel(8, 8, ST77XX_WHITE);
      faceCanvas.drawPixel(173, 8, ST77XX_WHITE);
    } else {
      faceCanvas.drawCircle(15, 10, 3, AMBER_COLOR);
      faceCanvas.drawCircle(166, 10, 3, AMBER_COLOR);
    }
  } else if (interaction == ROBOT_DANCE) {
    // DANÇA: oito poses alternadas, olhos piscando, pupilas deslizando,
    // boca cantando e notas musicais deslocadas a cada quadro.
    const uint8_t phase = static_cast<uint8_t>((nowMs / 120U) % 8U);
    const int8_t bob = (phase % 2U) ? 3 : -3;
    const int8_t sway = (phase < 2U || phase >= 6U) ? -4 :
                        (phase >= 3U && phase <= 4U) ? 4 : 0;
    const uint16_t danceA = color565(60, 225, 255);
    const uint16_t danceB = color565(255, 85, 210);
    const uint16_t danceC = color565(255, 210, 70);

    // Pálpebras coloridas e pupilas com brilho.
    if (phase == 1U || phase == 5U) {
      faceCanvas.fillRoundRect(leftEye - 21, 18 + bob, 42, 17, 7, danceA);
      faceCanvas.drawLine(rightEye - 19, 22 - bob, rightEye + 19, 22 - bob, danceB);
      faceCanvas.drawLine(rightEye - 15, 24 - bob, rightEye + 15, 24 - bob, danceB);
    } else if (phase == 3U || phase == 7U) {
      faceCanvas.drawLine(leftEye - 19, 22 - bob, leftEye + 19, 22 - bob, danceA);
      faceCanvas.drawLine(leftEye - 15, 24 - bob, leftEye + 15, 24 - bob, danceA);
      faceCanvas.fillRoundRect(rightEye - 21, 18 + bob, 42, 17, 7, danceB);
    } else {
      faceCanvas.fillRoundRect(leftEye - 21 + sway, 11 + bob, 42, 30, 10, danceA);
      faceCanvas.fillRoundRect(rightEye - 21 - sway, 11 - bob, 42, 30, 10, danceB);
      faceCanvas.fillRoundRect(leftEye - 15 + sway, 15 + bob, 30, 22, 7, BG_COLOR);
      faceCanvas.fillRoundRect(rightEye - 15 - sway, 15 - bob, 30, 22, 7, BG_COLOR);
      faceCanvas.fillCircle(leftEye + sway + ((phase % 3U) - 1) * 3, 25 + bob, 6, color565(0, 145, 190));
      faceCanvas.fillCircle(rightEye - sway + ((phase % 3U) - 1) * 3, 25 - bob, 6, color565(145, 35, 145));
      faceCanvas.fillCircle(leftEye + sway - 2, 22 + bob, 2, ST77XX_WHITE);
      faceCanvas.fillCircle(rightEye - sway - 2, 22 - bob, 2, ST77XX_WHITE);
    }

    // Boca muda entre sorriso e canto de boca aberta.
    if ((phase % 2U) == 0U) {
      faceCanvas.fillRoundRect(78, 43 - bob, 27, 11 + bob, 5, danceB);
      faceCanvas.fillRoundRect(83, 44 - bob, 17, 4, 2, BG_COLOR);
    } else {
      faceCanvas.drawLine(77, 45 + bob, 85, 51 + bob, danceA);
      faceCanvas.drawLine(85, 51 + bob, 94, 53 + bob, danceA);
      faceCanvas.drawLine(94, 53 + bob, 103, 51 + bob, danceA);
      faceCanvas.drawLine(103, 51 + bob, 111, 45 + bob, danceA);
    }

    // Notas musicais saltando entre os cantos, mais pequenos brilhos de festa.
    const int16_t noteY = (phase % 2U) ? 8 : 13;
    const int16_t leftNoteX = 8 + static_cast<int16_t>((phase % 4U) * 3U);
    const int16_t rightNoteX = 161 - static_cast<int16_t>((phase % 4U) * 3U);
    faceCanvas.fillCircle(leftNoteX, noteY + 15, 3, danceC);
    faceCanvas.drawLine(leftNoteX + 2, noteY + 13, leftNoteX + 2, noteY, danceC);
    faceCanvas.drawLine(leftNoteX + 2, noteY, leftNoteX + 8, noteY + 2, danceC);
    faceCanvas.fillCircle(rightNoteX, noteY + 15, 3, danceB);
    faceCanvas.drawLine(rightNoteX + 2, noteY + 13, rightNoteX + 2, noteY, danceB);
    faceCanvas.drawLine(rightNoteX + 2, noteY, rightNoteX + 8, noteY + 2, danceB);
    faceCanvas.drawPixel(25, 49, ST77XX_WHITE);
    faceCanvas.drawPixel(154, 48, ST77XX_WHITE);
  } else {
    switch (state) {
      case UI_BOOT: {
      int16_t eyeHeight = 4 + static_cast<int16_t>(elapsedMs / 12U);
      if (eyeHeight > 27) eyeHeight = 27;
      const int16_t y = eyeY - eyeHeight / 2;

      faceCanvas.fillRoundRect(leftEye - 21, y, 42, eyeHeight, 9, CYAN_COLOR);
      faceCanvas.fillRoundRect(rightEye - 21, y, 42, eyeHeight, 9, CYAN_COLOR);
      if (eyeHeight >= 13) {
        faceCanvas.fillCircle(leftEye, eyeY, 6, BG_COLOR);
        faceCanvas.fillCircle(rightEye, eyeY, 6, BG_COLOR);
        faceCanvas.fillCircle(leftEye - 2, eyeY - 3, 2, ST77XX_WHITE);
        faceCanvas.fillCircle(rightEye - 2, eyeY - 3, 2, ST77XX_WHITE);
      }
      faceCanvas.drawLine(85, 50, 102, 50, accent);
      break;
    }

    case UI_WAITING: {
      const uint8_t phase = static_cast<uint8_t>((nowMs / 350U) % 3U);
      int8_t shift = 0;
      if (phase == 0) shift = -5;
      else if (phase == 2) shift = 5;

      if (waitingBlink) {
        faceCanvas.drawLine(leftEye - 20, eyeY, leftEye + 20, eyeY, CYAN_COLOR);
        faceCanvas.drawLine(rightEye - 20, eyeY, rightEye + 20, eyeY, CYAN_COLOR);
        faceCanvas.drawLine(leftEye - 15, eyeY + 2, leftEye + 15, eyeY + 2, CYAN_COLOR);
        faceCanvas.drawLine(rightEye - 15, eyeY + 2, rightEye + 15, eyeY + 2, CYAN_COLOR);
      } else {
        for (int16_t cx : {leftEye, rightEye}) {
          faceCanvas.fillRoundRect(cx - 21, 10, 42, 29, 9, CYAN_COLOR);
          faceCanvas.fillRoundRect(cx - 16, 14, 32, 21, 7, BG_COLOR);
          faceCanvas.fillCircle(cx + shift, eyeY, 7, color565(0, 165, 210));
          faceCanvas.fillCircle(cx + shift - 2, eyeY - 3, 2, ST77XX_WHITE);
          faceCanvas.drawPixel(cx + shift + 2, eyeY + 3, color565(90, 230, 255));
        }
      }
      faceCanvas.fillCircle(85, 51, 2, accent);
      faceCanvas.fillCircle(91, 51, 2, accent);
      faceCanvas.fillCircle(97, 51, 2, accent);
      break;
    }

    case UI_CONNECTED:
      if (normalBlink) {
        faceCanvas.drawLine(leftEye - 19, 22, leftEye + 19, 22, GREEN_COLOR);
        faceCanvas.drawLine(rightEye - 19, 22, rightEye + 19, 22, GREEN_COLOR);
      } else {
        for (int16_t dx = -18; dx <= 18; ++dx) {
          const int16_t y = 15 + ((dx * dx) / 23);
          faceCanvas.drawLine(leftEye + dx, y, leftEye + dx, y + 2, GREEN_COLOR);
          faceCanvas.drawLine(rightEye + dx, y, rightEye + dx, y + 2, GREEN_COLOR);
        }
      }
      faceCanvas.drawPixel(22, 37, RED_COLOR);
      faceCanvas.drawPixel(25, 39, RED_COLOR);
      faceCanvas.drawPixel(157, 39, RED_COLOR);
      faceCanvas.drawPixel(160, 37, RED_COLOR);
      faceCanvas.drawLine(79, 47, 84, 52, GREEN_COLOR);
      faceCanvas.drawLine(84, 52, 91, 54, GREEN_COLOR);
      faceCanvas.drawLine(91, 54, 98, 52, GREEN_COLOR);
      faceCanvas.drawLine(98, 52, 103, 47, GREEN_COLOR);
      break;

    case UI_PUMP_ON: {
      const bool pulse = ((nowMs / 450U) % 2U) == 0U;
      const uint16_t eyeColor = pulse ? GREEN_COLOR : color565(0, 185, 110);
      faceCanvas.fillTriangle(26, 9, 68, 16, 66, 20, eyeColor);
      faceCanvas.fillTriangle(156, 9, 114, 16, 116, 20, eyeColor);
      for (int16_t cx : {leftEye, rightEye}) {
        faceCanvas.fillRoundRect(cx - 19, 14, 38, 25, 7, eyeColor);
        faceCanvas.fillCircle(cx, eyeY, 7, BG_COLOR);
        faceCanvas.fillCircle(cx - 2, eyeY - 3, 2, ST77XX_WHITE);
      }
      faceCanvas.drawLine(81, 51, 101, 51, eyeColor);
      faceCanvas.drawFastVLine(75, 45, 7, eyeColor);
      faceCanvas.drawFastVLine(107, 45, 7, eyeColor);
      break;
    }

    case UI_PUMP_OFF:
      faceCanvas.fillRoundRect(leftEye - 20, 12, 40, 25, 9, MUTED_COLOR);
      faceCanvas.fillRoundRect(rightEye - 20, 12, 40, 25, 9, MUTED_COLOR);
      faceCanvas.fillRect(leftEye - 19, 12, 38, 12, FACE_COLOR);
      faceCanvas.fillRect(rightEye - 19, 12, 38, 12, FACE_COLOR);
      faceCanvas.drawLine(leftEye - 18, 24, leftEye + 18, 24, ST77XX_WHITE);
      faceCanvas.drawLine(rightEye - 18, 24, rightEye + 18, 24, ST77XX_WHITE);
      faceCanvas.drawLine(83, 51, 99, 51, MUTED_COLOR);
      break;

    case UI_DISCONNECTED:
      faceCanvas.drawLine(29, 12, 47, 18, RED_COLOR);
      faceCanvas.drawLine(55, 18, 69, 12, RED_COLOR);
      faceCanvas.drawLine(113, 12, 127, 18, RED_COLOR);
      faceCanvas.drawLine(135, 18, 153, 12, RED_COLOR);
      faceCanvas.fillRoundRect(leftEye - 17, 17, 34, 21, 7, RED_COLOR);
      faceCanvas.fillRoundRect(rightEye - 17, 17, 34, 21, 7, RED_COLOR);
      faceCanvas.fillCircle(leftEye, 25, 6, BG_COLOR);
      faceCanvas.fillCircle(rightEye, 25, 6, BG_COLOR);
      faceCanvas.fillTriangle(35, 37, 41, 37, 38, 45, color565(40, 170, 255));
      faceCanvas.fillTriangle(141, 37, 147, 37, 144, 45, color565(40, 170, 255));
      faceCanvas.drawLine(80, 54, 85, 50, RED_COLOR);
      faceCanvas.drawLine(85, 50, 97, 50, RED_COLOR);
      faceCanvas.drawLine(97, 50, 102, 54, RED_COLOR);
      break;
    }
  }

  faceCanvas.fillCircle(2, 5, 1, accent);
  faceCanvas.fillCircle(180, 5, 1, accent);

  // Transferência em bloco da imagem já composta: reduz bastante o cintilar.
  tft.drawRGBBitmap(FACE_AREA_X, FACE_AREA_Y, faceCanvas.getBuffer(),
                    FACE_AREA_W, FACE_AREA_H);
}

static void drawStaticScreen(UiState state, RobotInteraction interaction) {
  if (!tftReady) return;

  const uint16_t accent = (interaction == ROBOT_ANGRY) ? RED_COLOR :
                          (interaction == ROBOT_DANCE) ? color565(255, 90, 220) :
                          statusColor(state);

  // A tela inteira só é limpa quando muda o estado do sistema.
  tft.fillScreen(BG_COLOR);

  // Fundo técnico sutil, sem animação nem efeito estroboscópico.
  tft.drawFastHLine(7, 16, 226, color565(24, 46, 62));
  tft.drawFastHLine(7, 96, 226, color565(24, 46, 62));
  tft.drawPixel(8, 8, accent);
  tft.drawPixel(12, 8, accent);
  tft.drawPixel(8, 12, accent);
  tft.drawPixel(231, 8, accent);
  tft.drawPixel(227, 8, accent);
  tft.drawPixel(231, 12, accent);

  // Cabeçalho com identidade do controlador.
  tft.fillRoundRect(17, 3, 13, 10, 3, accent);
  tft.fillCircle(23, 8, 2, BG_COLOR);
  tft.setTextSize(1);
  tft.setTextWrap(false);
  tft.setTextColor(ST77XX_WHITE, BG_COLOR);
  tft.setCursor(35, 5);
  tft.print("LUBRIFICA-O");
  tft.setTextColor(interaction == ROBOT_ANGRY ? RED_COLOR :
                   interaction == ROBOT_DANCE ? color565(255, 90, 220) :
                   MUTED_COLOR, BG_COLOR);
  tft.setCursor(167, 5);
  if (interaction == ROBOT_ANGRY) tft.print("BRAVO!");
  else if (interaction == ROBOT_DANCE) tft.print("DANCA!");
  else tft.print("HF-28 / BLE");

  // Corpo/visor com borda dupla, cantos e antena do robo.
  tft.fillRoundRect(17, 20, 206, 72, 14, FACE_COLOR);
  tft.drawRoundRect(15, 18, 210, 76, 15, accent);
  tft.drawRoundRect(19, 22, 202, 68, 11, color565(38, 74, 91));
  tft.drawLine(120, 18, 120, 13, accent);
  tft.fillCircle(120, 11, 3, accent);
  tft.fillCircle(120, 11, 1, ST77XX_WHITE);

  // Rodapé de estado com indicador colorido e texto nítido.
  tft.fillRoundRect(17, 101, 206, 25, 7, PANEL_COLOR);
  tft.drawRoundRect(17, 101, 206, 25, 7, accent);
  tft.fillCircle(28, 113, 3, accent);
  tft.drawCircle(28, 113, 5, color565(35, 60, 77));
  drawCenteredStatusText(uiLabel(state), 109, accent);

  // Quatro segmentos decorativos na base, como indicador de energia.
  const uint16_t dim = color565(28, 52, 69);
  tft.fillRect(93, 130, 10, 2, dim);
  tft.fillRect(106, 130, 10, 2, dim);
  tft.fillRect(119, 130, 10, 2, dim);
  tft.fillRect(132, 130, 10, 2, dim);
}

// Chave de animação: só atualiza os olhos quando algum detalhe mudou.
static uint32_t faceAnimationKey(UiState state, RobotInteraction interaction, uint32_t nowMs) {
  if (interaction == ROBOT_ANGRY) return nowMs / 115U;
  if (interaction == ROBOT_DANCE) return nowMs / 120U;
  switch (state) {
    case UI_BOOT:
      return nowMs / 80U;
    case UI_WAITING:
      return ((nowMs / 350U) << 1) | ((nowMs % 4700U) < 150U ? 1U : 0U);
    case UI_CONNECTED:
      return (nowMs % 3900U) < 150U ? 1U : 0U;
    case UI_PUMP_ON:
      return (((nowMs / 450U) % 2U) << 1) |
             ((nowMs % 4200U) < 150U ? 1U : 0U);
    case UI_PUMP_OFF:
      return (nowMs % 5200U) < 150U ? 1U : 0U;
    case UI_DISCONNECTED:
    default:
      return 0;
  }
}

// --------------------------- Relay / failsafe -------------------------------

static void setRelay(bool turnOn) {
  // GPIO 26 é ativo em HIGH conforme o módulo informado.
  digitalWrite(RELAY_PIN, turnOn ? HIGH : LOW);
  relayActive = turnOn;

  // O valor legível da característica acompanha o estado efetivo do relé.
  if (commandCharacteristic != nullptr) {
    commandCharacteristic->setValue(turnOn ? "1" : "0");
  }
}

static void forcePumpOff(const char* reason) {
  // Chamada também no callback de desconexão: não depende do loop() nem do OLED.
  setRelay(false);
  Serial.printf("[FAILSAFE] Relé em LOW: %s\n", reason);
}

// --------------------------- Callbacks de segurança -------------------------

class PumpSecurityCallbacks : public BLESecurityCallbacks {
 public:
  uint32_t onPassKeyRequest() override {
    return BLE_STATIC_PIN;
  }

  void onPassKeyNotify(uint32_t passKey) override {
    // O PIN é estático; não precisa ser exibido no OLED nem no log.
    (void)passKey;
  }

  bool onConfirmPIN(uint32_t passKey) override {
    return passKey == BLE_STATIC_PIN;
  }

  bool onSecurityRequest() override {
    return true;
  }

#if defined(CONFIG_BLUEDROID_ENABLED)
  void onAuthenticationComplete(esp_ble_auth_cmpl_t result) override {
    if (result.success) {
      Serial.println("[BLE] Pareamento/autenticacao concluido.");
    } else {
      Serial.printf("[BLE] Falha no pareamento (motivo %u).\n",
                    static_cast<unsigned>(result.fail_reason));
      bleConnected = false;
      forcePumpOff("falha de autenticacao BLE");
      setUiState(UI_DISCONNECTED);
    }
  }
#endif
};

// --------------------------- Callbacks do servidor -------------------------

class PumpServerCallbacks : public BLEServerCallbacks {
 public:
  void onConnect(BLEServer* server) override {
    (void)server;

    // Cada nova sessão começa com a bomba desligada; não herdar estado anterior.
    forcePumpOff("inicio de nova sessao BLE");
    bleConnected = true;
    setUiState(UI_CONNECTED);
    Serial.println("[BLE] Tablet conectado.");
  }

  void onDisconnect(BLEServer* server) override {
    (void)server;

    // FAILSAFE PRINCIPAL: desliga imediatamente quando a conexão cai.
    bleConnected = false;
    forcePumpOff("desconexao/perda de sinal BLE");
    setUiState(UI_DISCONNECTED);

    Serial.println("[BLE] Tablet desconectado; retomando advertising.");
    BLEDevice::startAdvertising();
  }
};

// --------------------------- Escrita da característica ---------------------

class PumpCharacteristicCallbacks : public BLECharacteristicCallbacks {
 public:
  void onWrite(BLECharacteristic* characteristic) override {
    // getValue() retorna String nas versões Arduino-ESP32 3.x usuais.
    const String value = characteristic->getValue();

    // A segurança da característica exige conexão criptografada com MITM.
    // Além disso, descartamos comandos se o servidor não estiver conectado.
    if (!bleConnected) {
      forcePumpOff("comando ignorado sem conexao BLE");
      return;
    }

    if (value.length() != 1) {
      Serial.println("[BLE] Comando invalido: esperado exatamente '1' ou '0'.");
      characteristic->setValue(relayActive ? "1" : "0");
      return;
    }

    if (value[0] == '1') {
      setRelay(true);
      setUiState(UI_PUMP_ON);
      Serial.println("[CMD] 1 recebido: bomba LIBERADA (GPIO 26 HIGH).");
      return;
    }

    if (value[0] == '0') {
      setRelay(false);
      setUiState(UI_PUMP_OFF);
      Serial.println("[CMD] 0 recebido: bomba TRAVADA (GPIO 26 LOW).");
      return;
    }

    Serial.println("[BLE] Comando desconhecido; relé não alterado.");
    characteristic->setValue(relayActive ? "1" : "0");
  }
};

// --------------------------- Inicialização do OLED --------------------------

static void initTFT() {
  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);  // Liga a iluminação da tela.

  // SPI do display da TTGO T-Display.
  SPI.begin(TFT_SCLK_PIN, -1, TFT_MOSI_PIN, TFT_CS_PIN);
  tft.init(TFT_NATIVE_WIDTH, TFT_NATIVE_HEIGHT);
  tft.setRotation(1);
  tft.setTextWrap(false);

  if (faceCanvas.getBuffer() == nullptr) {
    Serial.println("[TFT] Sem memoria para o buffer do rosto; mantendo o BLE ativo.");
    tftReady = false;
    return;
  }

  tftReady = true;
  Serial.printf("[TFT] ST7789 iniciado: %u x %u; MOSI=%u SCLK=%u CS=%u DC=%u RST=%u BL=%u.\n",
                static_cast<unsigned>(tft.width()),
                static_cast<unsigned>(tft.height()),
                TFT_MOSI_PIN, TFT_SCLK_PIN, TFT_CS_PIN, TFT_DC_PIN,
                TFT_RST_PIN, TFT_BL_PIN);

  drawStaticScreen(UI_BOOT);
  for (uint8_t frame = 0; frame < 5; ++frame) {
    drawFaceExpression(UI_BOOT, static_cast<uint32_t>(frame) * 90U, millis());
    delay(70);
  }
}

// --------------------------- Inicialização do BLE ---------------------------

static void initBLE() {
  BLEDevice::init(BLE_DEVICE_NAME);

  // PIN estático de 6 dígitos + Secure Connections + MITM + bonding.
  // A API antiga setStaticPIN é usada até o Core 3.3.0.
  BLESecurity security;
#if ESP_ARDUINO_VERSION <= ESP_ARDUINO_VERSION_VAL(3, 3, 0)
  security.setStaticPIN(BLE_STATIC_PIN);
  security.setCapability(ESP_IO_CAP_OUT);
  security.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  security.setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
#else
  BLESecurity::setPassKey(true, BLE_STATIC_PIN);
  BLESecurity::setCapability(ESP_IO_CAP_OUT);
  BLESecurity::setAuthenticationMode(true, true, true);
  BLESecurity::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
#endif

  BLEDevice::setSecurityCallbacks(new PumpSecurityCallbacks());

  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new PumpServerCallbacks());

  BLEService* service = bleServer->createService(SERVICE_UUID);

  uint32_t properties = BLECharacteristic::PROPERTY_READ |
                        BLECharacteristic::PROPERTY_WRITE;

#if defined(CONFIG_NIMBLE_ENABLED)
  // No NimBLE, estas propriedades protegem diretamente o acesso à característica.
  properties |= BLECharacteristic::PROPERTY_READ_AUTHEN |
                BLECharacteristic::PROPERTY_WRITE_AUTHEN;
#endif

  commandCharacteristic = service->createCharacteristic(
      CHARACTERISTIC_UUID,
      properties
  );
  commandCharacteristic->setCallbacks(new PumpCharacteristicCallbacks());
  commandCharacteristic->setValue("0");

#if defined(CONFIG_BLUEDROID_ENABLED)
  // No Bluedroid (stack padrão do ESP32), a proteção é definida nas permissões GATT.
  commandCharacteristic->setAccessPermissions(
      ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM
  );
#endif

  service->start();

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMaxPreferred(0x12);

  setUiState(UI_WAITING);
  BLEDevice::startAdvertising();

  Serial.println("[BLE] Servidor Bomba_HF28 anunciado.");
  Serial.println("[BLE] Aguardando tablet; PIN de pareamento: 123456.");
}

// --------------------------- Arduino setup/loop -----------------------------

void setup() {
  Serial.begin(115200);

  // Força o relé para OFF logo no início, antes de inicializar periféricos.
  digitalWrite(RELAY_PIN, LOW);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  relayActive = false;

  setUiState(UI_BOOT);
  initRobotButtons();
  Serial.println("\n[BOOT] Iniciando controlador da bomba HF-28.");

  // Prioridade operacional: anunciar o BLE primeiro, antes de inicializar a tela TFT.
  initBLE();

  // Display TFT inicializado depois do BLE, para priorizar a comunicacao.
  initTFT();

  Serial.println("[BOOT] Sistema pronto.");
}

void loop() {
  const uint32_t now = millis();

  // Botões alteram somente as expressões do robô; não acionam a bomba.
  updateRobotInteractions(now);

  // Segunda camada de fail-safe mantida intacta.
  if (!bleConnected && relayActive) {
    forcePumpOff("verificacao de seguranca no loop");
  }

  if (tftReady) {
    UiSnapshot snapshot = getUiSnapshot();

    static UiState lastDrawnState = static_cast<UiState>(255);
    static RobotInteraction lastDrawnInteraction = static_cast<RobotInteraction>(255);
    static uint32_t lastFaceKey = UINT32_MAX;

    const bool layoutChanged =
        (snapshot.state != lastDrawnState) ||
        (robotInteraction != lastDrawnInteraction);

    if (layoutChanged) {
      drawStaticScreen(snapshot.state, robotInteraction);
      drawFaceExpression(snapshot.state, robotInteraction,
                         now - snapshot.changedAt, now);
      lastDrawnState = snapshot.state;
      lastDrawnInteraction = robotInteraction;
      lastFaceKey = faceAnimationKey(snapshot.state, robotInteraction, now);
    } else {
      // Atualiza o rosto em buffer RAM; o resto da tela fica estático.
      const uint32_t key = faceAnimationKey(snapshot.state, robotInteraction, now);
      if (key != lastFaceKey) {
        drawFaceExpression(snapshot.state, robotInteraction,
                           now - interactionStartedAt, now);
        lastFaceKey = key;
      }
    }

    // Após 3 s, a expressão de perda de conexão retorna à busca.
    if (snapshot.state == UI_DISCONNECTED &&
        (now - snapshot.changedAt) >= 3000U) {
      setUiState(UI_WAITING);
    }
  }

  delay(10);
}
