# Firmware ESP32 — Bomba HF-28

Este diretório contém o firmware que conecta o PWA do projeto Lubrifica-o à placa LILYGO TTGO T-Display por Web Bluetooth (BLE). Ele foi mantido separado dos arquivos HTML/JS da aplicação publicada.

## Compatibilidade com o PWA

A implementação corresponde à versão em `docs/index.html`:
- Nome BLE anunciado: `Bomba_HF28`
- Serviço: `0000aaaa-0000-1000-8000-00805f9b34fb`
- Característica READ/WRITE: `0000bbbb-0000-1000-8000-00805f9b34fb`
- O PWA escreve o byte/string `1` para ligar e `0` para desligar.

## Arquivo

- `esp32_bomba_hf28.ino`: firmware completo, callbacks BLE, segurança do pareamento, fail-safe e interface animada da TFT.

## Hardware

| Sinal | ESP32 |
|---|---:|
| Entrada de comando do módulo de relé | GPIO 26 |
| Display TFT ST7789 — MOSI | GPIO 19 |
| Display TFT ST7789 — SCLK | GPIO 18 |
| Display TFT ST7789 — CS | GPIO 5 |
| Display TFT ST7789 — DC | GPIO 16 |
| Display TFT ST7789 — RST | GPIO 23 |
| Display TFT — backlight (BL) | GPIO 4 |

O relé foi solicitado como ativo em nível HIGH: GPIO 26 HIGH liga e LOW desliga. Confirme essa lógica no módulo real antes de ligar a bomba.

A placa mostrada no projeto é a LILYGO TTGO T-Display clássica: ela usa uma tela TFT colorida ST7789V de 240 × 135 pixels por SPI, e não um OLED SSD1306 por I²C. O firmware inicializa o backlight no GPIO 4 e configura os sinais SPI internos da placa. No Monitor Serial, procure a linha `[TFT] ST7789 iniciado`.

## Bibliotecas / compilação

1. No Arduino IDE, selecione uma placa ESP32 compatível (por exemplo, ESP32 Dev Module) e Arduino-ESP32 Core 3.x.
2. Instale **Adafruit GFX Library** e **Adafruit ST7735 and ST7789 Library** pelo Library Manager.
3. Abra `esp32_bomba_hf28.ino` e faça upload.
4. Abra o Monitor Serial em 115200 baud.
5. No Android, abra o PWA em contexto seguro (HTTPS), toque no botão de conexão Bluetooth e selecione `Bomba_HF28`. Quando o sistema solicitar o pareamento, use o PIN `123456`.

O BLE é inicializado antes da tela. A interface foi redesenhada para evitar oscilação: o layout completo só é redesenhado quando o estado muda; as animações atualizam apenas a região interna do rosto, sem limpar a tela toda a cada frame.

## Expressões da tela TFT

- Boot: olhos se abrindo progressivamente — `Iniciando Sistema`
- Busca: olhos com pupilas móveis e piscada ocasional — `Aguardando Bluetooth`
- Conexão: olhos sorridentes, bochechas e sorriso — `Conexao Estabelecida!`
- Comando `1`: olhar focado com indicadores de ação — `BOMBA LIBERADA`
- Comando `0`: olhos semicerrados e expressão neutra — `Bomba Travada`
- Desconexão: olhos tristes com lágrimas estilizadas — `Conexao Perdida` por 3 s, depois volta à busca.

O rosto usa um painel duplo de alto contraste, paleta por estado, cabeçalho e indicador inferior. A tela inteira não é redesenhada nos ciclos de animação; somente os pixels da área facial são atualizados.

## Segurança operacional

O firmware coloca o GPIO 26 em LOW no boot, em toda desconexão BLE, em falha de autenticação e se o loop detectar relé ligado sem cliente conectado. A característica exige criptografia/MITM e o pareamento solicita Secure Connections, MITM e bonding.

**Isso não substitui um circuito de segurança industrial.** Antes de acionar uma bomba real, valide em bancada o comportamento do relé durante reset, queda de alimentação, perda de rádio e falha do ESP32. Use um estágio de acionamento dimensionado para a carga, supressão de transientes, proteção elétrica e uma parada de emergência/intertravamento independente do software. Um PIN fixo é conhecido por todos que o conheçam e deve ser trocado/gerenciado se o ambiente exigir controle de acesso mais forte.
