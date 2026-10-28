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

static uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
  return tft.color565(r, g, b);
}

static void drawCenteredText(const char* text, int16_t y, uint16_t color) {
  tft.setTextWrap(false);
  int16_t x1=0,y1=0;
  uint16_t w=0,h=0;
  uint8_t scale=2;
  tft.setTextSize(scale);
  tft.getTextBounds(text,0,y,&x1,&y1,&w,&h);
  if(w>230) {
    scale=1;
    tft.setTextSize(scale);
    tft.getTextBounds(text,0,y,&x1,&y1,&w,&h);
  }
  int16_t x=(static_cast<int16_t>(tft.width())-static_cast<int16_t>(w))/2;
  if(x<0) x=0;
  tft.setTextColor(color,ST77XX_BLACK);
  tft.setCursor(x,y);
  tft.print(text);
}

static void drawNeutralEye(int16_t cx,int8_t shift,bool blink,uint16_t eyeColor) {
  if(blink) {
    tft.drawLine(cx-20,51,cx+20,51,eyeColor);
    tft.drawLine(cx-16,53,cx+16,53,eyeColor);
    return;
  }
  tft.fillRoundRect(cx-21,36,42,30,9,eyeColor);
  tft.fillCircle(cx+shift,51,7,ST77XX_BLACK);
  tft.fillCircle(cx+shift-2,48,2,ST77XX_WHITE);
}

static void drawHappyEye(int16_t cx,uint16_t eyeColor) {
  for(int16_t dx=-19;dx<=19;++dx) {
    const int16_t y=42+((dx*dx)/24);
    tft.drawPixel(cx+dx,y,eyeColor);
    tft.drawPixel(cx+dx,y+1,eyeColor);
    if(dx%3==0) tft.drawPixel(cx+dx,y+2,eyeColor);
  }
}

static void drawFocusedEyes(uint16_t eyeColor) {
  tft.drawLine(57,30,91,36,eyeColor);
  tft.drawLine(149,36,183,30,eyeColor);
  tft.fillRoundRect(62,38,37,27,7,eyeColor);
  tft.fillRoundRect(141,38,37,27,7,eyeColor);
  tft.fillCircle(81,52,7,ST77XX_BLACK);
  tft.fillCircle(160,52,7,ST77XX_BLACK);
  tft.fillCircle(79,49,2,ST77XX_WHITE);
  tft.fillCircle(158,49,2,ST77XX_WHITE);
}

static void drawRestingEyes(uint16_t eyeColor) {
  tft.drawLine(62,49,99,49,eyeColor);
  tft.drawLine(141,49,178,49,eyeColor);
  tft.drawLine(66,52,95,52,eyeColor);
  tft.drawLine(145,52,174,52,eyeColor);
}

static void drawSadEyes(uint16_t eyeColor) {
  tft.drawLine(59,34,77,39,eyeColor);
  tft.drawLine(81,39,98,34,eyeColor);
  tft.drawLine(142,34,159,39,eyeColor);
  tft.drawLine(163,39,181,34,eyeColor);
  tft.fillRoundRect(63,40,34,25,7,eyeColor);
  tft.fillRoundRect(143,40,34,25,7,eyeColor);
  tft.fillCircle(82,51,6,ST77XX_BLACK);
  tft.fillCircle(162,51,6,ST77XX_BLACK);
  tft.fillTriangle(68,65,73,65,70,72,tft.color565(40,170,255));
  tft.fillTriangle(168,65,173,65,170,72,tft.color565(40,170,255));
}

static uint16_t statusColor(UiState state) {
  switch(state) {
    case UI_BOOT: return tft.color565(255,190,55);
    case UI_WAITING: return tft.color565(70,205,255);
    case UI_CONNECTED: return tft.color565(70,235,150);
    case UI_PUMP_ON: return tft.color565(70,235,150);
    case UI_PUMP_OFF: return tft.color565(175,190,210);
    case UI_DISCONNECTED: return tft.color565(255,90,100);
    default: return ST77XX_WHITE;
  }
}

static void drawRobot(UiState state,uint32_t elapsedMs,uint32_t nowMs) {
  if(!tftReady) return;
  const uint16_t accent=statusColor(state);
  const uint16_t faceColor=tft.color565(8,24,40);
  tft.fillScreen(ST77XX_BLACK);

  tft.setTextWrap(false);
  tft.setTextSize(1);
  tft.setTextColor(tft.color565(115,150,175),ST77XX_BLACK);
  tft.setCursor(8,4);
  tft.print("LUBRIFICA-O / HF28");

  tft.drawRoundRect(22,16,196,75,15,accent);
  tft.fillRoundRect(24,18,192,71,13,faceColor);
  tft.drawLine(120,18,120,11,accent);
  tft.fillCircle(120,9,4,accent);

  switch(state) {
    case UI_BOOT: {
      int16_t eyeHeight=3+static_cast<int16_t>(elapsedMs/14);
      if(eyeHeight>30) eyeHeight=30;
      const int16_t eyeY=51-(eyeHeight/2);
      tft.fillRoundRect(62,eyeY,38,eyeHeight,8,ST77XX_CYAN);
      tft.fillRoundRect(140,eyeY,38,eyeHeight,8,ST77XX_CYAN);
      if(eyeHeight>=13) {
        tft.fillCircle(81,51,6,ST77XX_BLACK);
        tft.fillCircle(159,51,6,ST77XX_BLACK);
      }
      break;
    }
    case UI_WAITING: {
      const uint8_t phase=static_cast<uint8_t>((nowMs/350)%4);
      int8_t shift=0;
      if(phase==0) shift=-5;
      if(phase==2) shift=5;
      const bool blink=(nowMs%4200)<150;
      drawNeutralEye(81,shift,blink,ST77XX_CYAN);
      drawNeutralEye(159,shift,blink,ST77XX_CYAN);
      break;
    }
    case UI_CONNECTED:
      drawHappyEye(81,tft.color565(70,235,150));
      drawHappyEye(159,tft.color565(70,235,150));
      tft.fillCircle(47,43,2,ST77XX_YELLOW);
      tft.fillCircle(193,43,2,ST77XX_YELLOW);
      break;
    case UI_PUMP_ON: drawFocusedEyes(tft.color565(70,235,150)); break;
    case UI_PUMP_OFF: drawRestingEyes(tft.color565(175,190,210)); break;
    case UI_DISCONNECTED: drawSadEyes(tft.color565(255,90,100)); break;
  }

  tft.drawFastHLine(28,97,184,tft.color565(38,58,75));
  drawCenteredText(uiLabel(state),108,accent);
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
  // A TTGO T-Display tem TFT ST7789 por SPI, nao OLED I2C.
  pinMode(TFT_BL_PIN,OUTPUT);
  digitalWrite(TFT_BL_PIN,HIGH);  // Liga a iluminacao da tela.

  // Display sem MISO: MOSI=19 e SCLK=18.
  SPI.begin(TFT_SCLK_PIN,-1,TFT_MOSI_PIN,TFT_CS_PIN);

  // Controlador nativo 135x240; rotacao 1 produz a interface 240x135.
  tft.init(TFT_NATIVE_WIDTH,TFT_NATIVE_HEIGHT);
  tft.setRotation(1);
  tft.setTextWrap(false);
  tft.fillScreen(ST77XX_BLACK);
  tftReady=true;

  Serial.printf("[TFT] TTGO T-Display ST7789 iniciado: %u x %u; MOSI=%u SCLK=%u CS=%u DC=%u RST=%u BL=%u.\n",
                static_cast<unsigned>(tft.width()),
                static_cast<unsigned>(tft.height()),
                TFT_MOSI_PIN,TFT_SCLK_PIN,TFT_CS_PIN,TFT_DC_PIN,TFT_RST_PIN,TFT_BL_PIN);

  // Animacao curta; o BLE ja foi inicializado antes desta funcao.
  for(uint8_t frame=0;frame<8;++frame) {
    drawRobot(UI_BOOT,static_cast<uint32_t>(frame)*70,millis());
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

  // Segunda camada de fail-safe: se o estado ficar inconsistente, OFF prevalece.
  if (!bleConnected && relayActive) {
    forcePumpOff("verificacao de seguranca no loop");
  }

  if (tftReady) {
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
