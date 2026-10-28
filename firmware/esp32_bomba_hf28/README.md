# Firmware ESP32 — Bomba HF-28

Este diretório contém o firmware que conecta o PWA do projeto Lubrifica-o à placa ESP32-WROOM-32E por Web Bluetooth (BLE). Ele foi mantido separado dos arquivos HTML/JS da aplicação publicada.

## Compatibilidade com o PWA

A implementação corresponde à versão em `docs/index.html`:
- Nome BLE anunciado: `Bomba_HF28`
- Serviço: `0000aaaa-0000-1000-8000-00805f9b34fb`
- Característica READ/WRITE: `0000bbbb-0000-1000-8000-00805f9b34fb`
- O PWA escreve o byte/string `1` para ligar e `0` para desligar.

## Arquivo

- `esp32_bomba_hf28.ino`: firmware completo, callbacks BLE, segurança do pareamento, fail-safe e expressões do OLED.

## Hardware

| Sinal | ESP32 |
|---|---:|
| Entrada de comando do módulo de relé | GPIO 26 |
| OLED I2C (pinagem detectada automaticamente) | GPIO 21/22, 4/15 ou 5/4 |
| OLED endereço I2C | 0x3C ou 0x3D |
| OLED alimentação | Conforme a placa/display |
| OLED GND | GND comum conforme especificação do módulo |

O relé foi solicitado como ativo em nível HIGH: GPIO 26 HIGH liga e LOW desliga. Confirme essa lógica no módulo real antes de ligar a bomba.

O firmware agora testa automaticamente três pinagens I²C comuns em placas TTGO/LILYGO com SSD1306: SDA 21/SCL 22, SDA 4/SCL 15 e SDA 5/SCL 4. No Monitor Serial, procure a linha `[OLED] SSD1306 encontrado` para saber quais pinos e endereço foram detectados.

**Atenção:** a TTGO T-Display usa normalmente uma tela TFT ST7789 por SPI, não SSD1306. Se o seu modelo for T-Display, este driver OLED não é compatível; será necessário adaptar a interface para TFT.

## Bibliotecas / compilação

1. No Arduino IDE, selecione uma placa ESP32 compatível (por exemplo, ESP32 Dev Module) e Arduino-ESP32 Core 3.x.
2. Instale **Adafruit GFX Library** e **Adafruit SSD1306** pelo Library Manager.
3. Abra `esp32_bomba_hf28.ino` e faça upload.
4. Abra o Monitor Serial em 115200 baud.
5. No Android, abra o PWA em contexto seguro (HTTPS), toque no botão de conexão Bluetooth e selecione `Bomba_HF28`. Quando o sistema solicitar o pareamento, use o PIN `123456`.

O OLED é opcional: o firmware faz uma detecção I2C finita e não entra em uma espera infinita caso o SSD1306 não esteja presente. O BLE é inicializado mesmo que o display não responda.

## Expressões do OLED

- Boot: olhos abrindo — `Iniciando Sistema`
- Busca: pupilas se movimentando — `Aguardando Bluetooth`
- Conexão: olhos sorrindo — `Conexao Estabelecida!`
- Comando `1`: olhos focados — `BOMBA LIBERADA`
- Comando `0`: olhos fechados — `Bomba Travada`
- Desconexão: olhos tristes — `Conexao Perdida` por 3 s, depois volta à busca.

## Segurança operacional

O firmware coloca o GPIO 26 em LOW no boot, em toda desconexão BLE, em falha de autenticação e se o loop detectar relé ligado sem cliente conectado. A característica exige criptografia/MITM e o pareamento solicita Secure Connections, MITM e bonding.

**Isso não substitui um circuito de segurança industrial.** Antes de acionar uma bomba real, valide em bancada o comportamento do relé durante reset, queda de alimentação, perda de rádio e falha do ESP32. Use um estágio de acionamento dimensionado para a carga, supressão de transientes, proteção elétrica e uma parada de emergência/intertravamento independente do software. Um PIN fixo é conhecido por todos que o conheçam e deve ser trocado/gerenciado se o ambiente exigir controle de acesso mais forte.
