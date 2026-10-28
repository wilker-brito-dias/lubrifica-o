/*
 * Lubrifica-o — Firmware ESP32 para a bomba HF-28
 * Placa: ESP32-WROOM-32E
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
 * OLED SSD1306:
 *   128x64, I2C SDA=GPIO 5 / SCL=GPIO 4, endereço normalmente 0x3C.
 *
 * IMPORTANTE:
 * - O relé começa em LOW e também é desligado em desconexão BLE.
 * - O OLED é opcional: ausência/falha não impede a inicialização do BLE.
 * - Para uma instalação industrial, use intertravamento/parada de emergência
 *   independente do ESP32 e driver apropriado para a bobina/carga do relé.
 */

#include <Arduino.h>
#include <esp_arduino_version.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLESecurity.h>

#if defined(CONFIG_BLUEDROID_ENABLED)
  #include <esp_gap_ble_api.h>
#endif

// --------------------------- Hardware ---------------------------------------

static constexpr uint8_t RELAY_PIN = 26;
static constexpr uint8_t OLED_SDA_PIN = 5;
static constexpr uint8_t OLED_SCL_PIN = 4;
static constexpr uint8_t OLED_WIDTH = 128;
static constexpr uint8_t OLED_HEIGHT = 64;
static constexpr uint8_t OLED_ADDR_1 = 0x3C;
static constexpr uint8_t OLED_ADDR_2 = 0x3D;

static constexpr uint32_t BLE_STATIC_PIN = 123456;
static constexpr uint32_t OLED_I2C_TIMEOUT_MS = 50;

Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
static bool oledReady = false;
static uint8_t oledAddress = OLED_ADDR_1;

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

static void drawCenteredText(const char* text, int16_t baselineY) {
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.getTextBounds(text, 0, baselineY, &x1, &y1, &w, &h);
  int16_t x = (static_cast<int16_t>(OLED_WIDTH) - static_cast<int16_t>(w)) / 2;
  if (x < 0) x = 0;
  oled.setCursor(x, baselineY);
  oled.print(text);
}

static void drawNeutralEye(int16_t centerX, int8_t pupilShift, bool blink) {
  if (blink) {
    oled.drawLine(centerX - 14, 23, centerX + 14, 23, SSD1306_WHITE);
    oled.drawLine(centerX - 11, 24, centerX + 11, 24, SSD1306_WHITE);
    return;
  }

  oled.fillRoundRect(centerX - 15, 11, 30, 24, 7, SSD1306_WHITE);
  oled.fillCircle(centerX + pupilShift, 23, 5, SSD1306_BLACK);
  oled.drawPixel(centerX + pupilShift - 1, 21, SSD1306_WHITE);
}

static void drawHappyEye(int16_t centerX) {
  // Arco em pixels: os olhos ficam fechados em um sorriso.
  for (int16_t dx = -12; dx <= 12; ++dx) {
    const int16_t y = 15 + ((dx * dx) / 12);
    oled.drawPixel(centerX + dx, y, SSD1306_WHITE);
    oled.drawPixel(centerX + dx, y + 1, SSD1306_WHITE);
  }
}

static void drawFocusedEyes() {
  // Sobrancelhas inclinadas + olhos abertos: expressão de ação/concentração.
  oled.drawLine(23, 8, 54, 13, SSD1306_WHITE);
  oled.drawLine(74, 13, 105, 8, SSD1306_WHITE);

  oled.fillRoundRect(25, 14, 29, 21, 5, SSD1306_WHITE);
  oled.fillRoundRect(74, 14, 29, 21, 5, SSD1306_WHITE);
  oled.fillCircle(40, 24, 5, SSD1306_BLACK);
  oled.fillCircle(88, 24, 5, SSD1306_BLACK);
  oled.drawPixel(39, 22, SSD1306_WHITE);
  oled.drawPixel(87, 22, SSD1306_WHITE);
}

static void drawRestingEyes() {
  // Pálpebras fechadas: a bomba está travada/desligada.
  oled.drawLine(25, 22, 54, 22, SSD1306_WHITE);
  oled.drawLine(74, 22, 103, 22, SSD1306_WHITE);
  oled.drawLine(28, 24, 51, 24, SSD1306_WHITE);
  oled.drawLine(77, 24, 100, 24, SSD1306_WHITE);
}

static void drawSadEyes() {
  // Sobrancelhas inclinadas para cima nas extremidades e pequenas lágrimas.
  oled.drawLine(24, 9, 39, 13, SSD1306_WHITE);
  oled.drawLine(89, 13, 104, 9, SSD1306_WHITE);

  oled.fillRoundRect(27, 15, 25, 18, 5, SSD1306_WHITE);
  oled.fillRoundRect(76, 15, 25, 18, 5, SSD1306_WHITE);
  oled.fillCircle(40, 23, 4, SSD1306_BLACK);
  oled.fillCircle(88, 23, 4, SSD1306_BLACK);

  oled.drawLine(30, 33, 28, 37, SSD1306_WHITE);
  oled.drawLine(98, 33, 100, 37, SSD1306_WHITE);
}

static void drawRobot(UiState state, uint32_t elapsedMs, uint32_t nowMs) {
  if (!oledReady) return; // Nunca tentar desenhar em um OLED não inicializado.

  oled.clearDisplay();

  // Moldura do rosto e uma pequena antena.
  oled.drawRoundRect(5, 4, 118, 42, 8, SSD1306_WHITE);
  oled.drawLine(64, 4, 64, 1, SSD1306_WHITE);
  oled.fillCircle(64, 1, 2, SSD1306_WHITE);

  switch (state) {
    case UI_BOOT: {
      // Abertura progressiva dos olhos durante a inicialização.
      int16_t eyeHeight = 3 + static_cast<int16_t>(elapsedMs / 20);
      if (eyeHeight > 23) eyeHeight = 23;
      const int16_t eyeY = 23 - (eyeHeight / 2);

      oled.fillRoundRect(25, eyeY, 29, eyeHeight, 6, SSD1306_WHITE);
      oled.fillRoundRect(74, eyeY, 29, eyeHeight, 6, SSD1306_WHITE);
      if (eyeHeight >= 13) {
        oled.fillCircle(40, 23, 4, SSD1306_BLACK);
        oled.fillCircle(88, 23, 4, SSD1306_BLACK);
      }
      break;
    }

    case UI_WAITING: {
      // Olhos procurando: pupilas se movem lateralmente e piscam periodicamente.
      const uint8_t phase = static_cast<uint8_t>((nowMs / 350) % 4);
      int8_t shift = 0;
      if (phase == 0) shift = -3;
      if (phase == 2) shift = 3;
      const bool blink = (nowMs % 4200) < 130;
      drawNeutralEye(40, shift, blink);
      drawNeutralEye(88, shift, blink);
      break;
    }

    case UI_CONNECTED:
      drawHappyEye(40);
      drawHappyEye(88);
      oled.drawPixel(21, 18, SSD1306_WHITE);
      oled.drawPixel(107, 18, SSD1306_WHITE);
      break;

    case UI_PUMP_ON:
      drawFocusedEyes();
      break;

    case UI_PUMP_OFF:
      drawRestingEyes();
      break;

    case UI_DISCONNECTED:
      drawSadEyes();
      break;
  }

  drawCenteredText(uiLabel(state), 51);
  oled.display();
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

static bool probeI2C(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

static void initOLED() {
  // Esta função não contém espera por hardware nem loop de tentativas.
  if (!Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN, 400000)) {
    Serial.println("[OLED] Wire.begin falhou; continuando sem display.");
    oledReady = false;
    return;
  }

  // Limita o tempo de espera caso o barramento I2C esteja travado.
  Wire.setTimeOut(OLED_I2C_TIMEOUT_MS);

  if (probeI2C(OLED_ADDR_1)) {
    oledAddress = OLED_ADDR_1;
  } else if (probeI2C(OLED_ADDR_2)) {
    oledAddress = OLED_ADDR_2;
  } else {
    Serial.println("[OLED] SSD1306 nao encontrado em 0x3C/0x3D; BLE segue ativo.");
    oledReady = false;
    return;
  }

  // periphBegin=false porque Wire já foi inicializado nos pinos personalizados.
  if (!oled.begin(SSD1306_SWITCHCAPVCC, oledAddress, true, false)) {
    Serial.println("[OLED] Falha ao alocar/inicializar SSD1306; BLE segue ativo.");
    oledReady = false;
    return;
  }

  oledReady = true;
  Serial.printf("[OLED] SSD1306 inicializado no endereco 0x%02X.\n", oledAddress);

  // Pequena animação finita de boot; se falhar, não bloqueia o serviço BLE.
  for (uint8_t frame = 0; frame < 6; ++frame) {
    drawRobot(UI_BOOT, static_cast<uint32_t>(frame) * 85, millis());
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

  // Prioridade operacional: anunciar o BLE primeiro. Mesmo que o OLED trave,
  // esteja ausente ou falhe na inicializacao, o servidor BLE ja estara ativo.
  initBLE();

  // O display e acessorio; uma falha aqui nao impede o atendimento BLE.
  initOLED();

  Serial.println("[BOOT] Sistema pronto.");
}

void loop() {
  const uint32_t now = millis();

  // Segunda camada de fail-safe: se o estado ficar inconsistente, OFF prevalece.
  if (!bleConnected && relayActive) {
    forcePumpOff("verificacao de seguranca no loop");
  }

  if (oledReady) {
    UiSnapshot snapshot = getUiSnapshot();
    static UiState lastDrawnState = static_cast<UiState>(255);
    static uint32_t lastDrawAt = 0;

    // Redesenha em pequenos intervalos para animar os olhos sem bloquear o BLE.
    if (snapshot.state != lastDrawnState || (now - lastDrawAt) >= 90) {
      drawRobot(snapshot.state, now - snapshot.changedAt, now);
      lastDrawnState = snapshot.state;
      lastDrawAt = now;
    }

    // Após mostrar a perda de conexão por 3 s, volta à expressão de busca.
    // O BLE continua anunciando durante todo esse tempo.
    if (snapshot.state == UI_DISCONNECTED &&
        (now - snapshot.changedAt) >= 3000) {
      setUiState(UI_WAITING);
    }
  }

  delay(10);
}
