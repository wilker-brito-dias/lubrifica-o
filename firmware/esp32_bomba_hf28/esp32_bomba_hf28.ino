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

Adafruit_ST7789 tft(&SPI, TFT_CS_PIN, TFT_DC_PIN, TFT_RST_PIN);
static bool tftReady = false;

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
static void drawFaceExpression(UiState state, uint32_t elapsedMs, uint32_t nowMs) {
  if (!tftReady) return;

  const uint16_t accent = statusColor(state);
  const bool waitingBlink = (nowMs % 4700U) < 150U;
  const bool normalBlink = (nowMs % 3900U) < 150U;
  const int16_t leftEye = 80;
  const int16_t rightEye = 160;
  const int16_t eyeY = 51;

  // Apaga somente o interior da face. As molduras e a interface permanecem
  // estáveis; isso elimina a piscada causada por fillScreen() a cada frame.
  tft.fillRect(29, 27, 182, 59, FACE_COLOR);

  // Pequenos detalhes laterais do rosto.
  tft.drawPixel(43, 43, accent);
  tft.drawPixel(197, 43, accent);

  switch (state) {
    case UI_BOOT: {
      int16_t eyeHeight = 4 + static_cast<int16_t>(elapsedMs / 12U);
      if (eyeHeight > 27) eyeHeight = 27;
      const int16_t y = eyeY - eyeHeight / 2;

      tft.fillRoundRect(leftEye - 21, y, 42, eyeHeight, 9, CYAN_COLOR);
      tft.fillRoundRect(rightEye - 21, y, 42, eyeHeight, 9, CYAN_COLOR);
      if (eyeHeight >= 13) {
        tft.fillCircle(leftEye, eyeY, 6, BG_COLOR);
        tft.fillCircle(rightEye, eyeY, 6, BG_COLOR);
        tft.fillCircle(leftEye - 2, eyeY - 3, 2, ST77XX_WHITE);
        tft.fillCircle(rightEye - 2, eyeY - 3, 2, ST77XX_WHITE);
      }
      tft.drawLine(109, 77, 131, 77, accent);
      break;
    }

    case UI_WAITING: {
      // O olhar procura alternando à esquerda, centro e direita; pisca raramente.
      const uint8_t phase = static_cast<uint8_t>((nowMs / 350U) % 3U);
      int8_t shift = 0;
      if (phase == 0) shift = -5;
      else if (phase == 2) shift = 5;

      if (waitingBlink) {
        tft.drawLine(leftEye - 20, eyeY, leftEye + 20, eyeY, CYAN_COLOR);
        tft.drawLine(rightEye - 20, eyeY, rightEye + 20, eyeY, CYAN_COLOR);
        tft.drawLine(leftEye - 15, eyeY + 2, leftEye + 15, eyeY + 2, CYAN_COLOR);
        tft.drawLine(rightEye - 15, eyeY + 2, rightEye + 15, eyeY + 2, CYAN_COLOR);
      } else {
        for (int16_t cx : {leftEye, rightEye}) {
          tft.fillRoundRect(cx - 21, 37, 42, 29, 9, CYAN_COLOR);
          tft.fillRoundRect(cx - 16, 41, 32, 21, 7, BG_COLOR);
          tft.fillCircle(cx + shift, eyeY, 7, color565(0, 165, 210));
          tft.fillCircle(cx + shift - 2, eyeY - 3, 2, ST77XX_WHITE);
          tft.drawPixel(cx + shift + 2, eyeY + 3, color565(90, 230, 255));
        }
      }
      // Boca curiosa, com três pontos.
      tft.fillCircle(114, 78, 2, accent);
      tft.fillCircle(120, 78, 2, accent);
      tft.fillCircle(126, 78, 2, accent);
      break;
    }

    case UI_CONNECTED:
      if (normalBlink) {
        tft.drawLine(leftEye - 19, 49, leftEye + 19, 49, GREEN_COLOR);
        tft.drawLine(rightEye - 19, 49, rightEye + 19, 49, GREEN_COLOR);
      } else {
        // Olhos sorridentes com arco grosso e pontas arredondadas.
        for (int16_t dx = -18; dx <= 18; ++dx) {
          const int16_t y = 42 + ((dx * dx) / 23);
          tft.drawLine(leftEye + dx, y, leftEye + dx, y + 2, GREEN_COLOR);
          tft.drawLine(rightEye + dx, y, rightEye + dx, y + 2, GREEN_COLOR);
        }
      }
      tft.drawPixel(51, 64, RED_COLOR);
      tft.drawPixel(54, 66, RED_COLOR);
      tft.drawPixel(186, 66, RED_COLOR);
      tft.drawPixel(189, 64, RED_COLOR);
      // Sorriso.
      tft.drawLine(108, 74, 113, 79, GREEN_COLOR);
      tft.drawLine(113, 79, 120, 81, GREEN_COLOR);
      tft.drawLine(120, 81, 127, 79, GREEN_COLOR);
      tft.drawLine(127, 79, 132, 74, GREEN_COLOR);
      break;

    case UI_PUMP_ON: {
      // Expressão focada: sobrancelhas inclinadas, íris grandes e indicadores de ação.
      const bool pulse = ((nowMs / 450U) % 2U) == 0U;
      const uint16_t eyeColor = pulse ? GREEN_COLOR : color565(0, 185, 110);
      tft.fillTriangle(55, 36, 97, 43, 95, 47, eyeColor);
      tft.fillTriangle(185, 36, 143, 43, 145, 47, eyeColor);
      for (int16_t cx : {leftEye, rightEye}) {
        tft.fillRoundRect(cx - 19, 41, 38, 25, 7, eyeColor);
        tft.fillCircle(cx, eyeY, 7, BG_COLOR);
        tft.fillCircle(cx - 2, eyeY - 3, 2, ST77XX_WHITE);
      }
      // Boca de esforço e dois pequenos traços de energia.
      tft.drawLine(110, 78, 130, 78, eyeColor);
      tft.drawFastVLine(104, 72, 7, eyeColor);
      tft.drawFastVLine(136, 72, 7, eyeColor);
      break;
    }

    case UI_PUMP_OFF:
      // Olhos descansando, com pálpebras superiores bem definidas.
      tft.fillRoundRect(leftEye - 20, 39, 40, 25, 9, MUTED_COLOR);
      tft.fillRoundRect(rightEye - 20, 39, 40, 25, 9, MUTED_COLOR);
      tft.fillRect(leftEye - 19, 39, 38, 12, FACE_COLOR);
      tft.fillRect(rightEye - 19, 39, 38, 12, FACE_COLOR);
      tft.drawLine(leftEye - 18, 51, leftEye + 18, 51, ST77XX_WHITE);
      tft.drawLine(rightEye - 18, 51, rightEye + 18, 51, ST77XX_WHITE);
      tft.drawLine(112, 78, 128, 78, MUTED_COLOR);
      break;

    case UI_DISCONNECTED:
      // Olhos tristes com sobrancelhas caídas e pequenas lágrimas.
      tft.drawLine(58, 39, 76, 45, RED_COLOR);
      tft.drawLine(84, 45, 98, 39, RED_COLOR);
      tft.drawLine(142, 39, 156, 45, RED_COLOR);
      tft.drawLine(164, 45, 182, 39, RED_COLOR);
      tft.fillRoundRect(leftEye - 17, 44, 34, 21, 7, RED_COLOR);
      tft.fillRoundRect(rightEye - 17, 44, 34, 21, 7, RED_COLOR);
      tft.fillCircle(leftEye, 52, 6, BG_COLOR);
      tft.fillCircle(rightEye, 52, 6, BG_COLOR);
      tft.fillTriangle(64, 64, 70, 64, 67, 72, color565(40, 170, 255));
      tft.fillTriangle(170, 64, 176, 64, 173, 72, color565(40, 170, 255));
      // Boca triste.
      tft.drawLine(109, 81, 114, 77, RED_COLOR);
      tft.drawLine(114, 77, 126, 77, RED_COLOR);
      tft.drawLine(126, 77, 131, 81, RED_COLOR);
      break;
  }

  // Mini LEDs e parafusos do painel do rosto.
  tft.fillCircle(31, 32, 1, accent);
  tft.fillCircle(209, 32, 1, accent);
}

static void drawStaticScreen(UiState state) {
  if (!tftReady) return;

  const uint16_t accent = statusColor(state);

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
  tft.setTextColor(MUTED_COLOR, BG_COLOR);
  tft.setCursor(167, 5);
  tft.print("HF-28 / BLE");

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
static uint32_t faceAnimationKey(UiState state, uint32_t nowMs) {
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
  Serial.println("\n[BOOT] Iniciando controlador da bomba HF-28.");

  // Prioridade operacional: anunciar o BLE primeiro, antes de inicializar a tela TFT.
  initBLE();

  // Display TFT inicializado depois do BLE, para priorizar a comunicacao.
  initTFT();

  Serial.println("[BOOT] Sistema pronto.");
}

void loop() {
  const uint32_t now = millis();

  // Segunda camada de fail-safe: OFF prevalece se não houver cliente BLE.
  if (!bleConnected && relayActive) {
    forcePumpOff("verificacao de seguranca no loop");
  }

  if (tftReady) {
    UiSnapshot snapshot = getUiSnapshot();

    static UiState lastDrawnState = static_cast<UiState>(255);
    static uint32_t lastFaceKey = UINT32_MAX;

    // Tela completa: somente no boot ou quando muda o estado da operação.
    if (snapshot.state != lastDrawnState) {
      drawStaticScreen(snapshot.state);
      drawFaceExpression(snapshot.state, now - snapshot.changedAt, now);
      lastDrawnState = snapshot.state;
      lastFaceKey = faceAnimationKey(snapshot.state, now);
    } else {
      // Animação localizada: apenas o interior do rosto, nunca fillScreen().
      const uint32_t key = faceAnimationKey(snapshot.state, now);
      if (key != lastFaceKey) {
        drawFaceExpression(snapshot.state, now - snapshot.changedAt, now);
        lastFaceKey = key;
      }
    }

    // Após 3 s, a expressão de perda de conexão retorna à busca BLE.
    if (snapshot.state == UI_DISCONNECTED &&
        (now - snapshot.changedAt) >= 3000U) {
      setUiState(UI_WAITING);
    }
  }

  delay(10);
}
