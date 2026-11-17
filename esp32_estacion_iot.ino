#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <DHT.h>
#include <time.h>

// === CONFIGURACIÓN WI-FI Y MQTT ===
const char* ssid = "TU_RED_WIFI";
const char* password = "TU_PASSWORD_WIFI";

const char* ap_ssid = "EstacionIoT-ESP32";
const char* ap_password = "12345678";

const bool HABILITAR_MQTT_NUBE     = true;
const char* MQTT_BROKER_PRIMARIO   = "broker.hivemq.com";
const char* MQTT_BROKER_SECUNDARIO = "broker.emqx.io";
const uint16_t MQTT_PORT           = 1883;
const char* MQTT_TOPIC             = "inacap/estacion_iot_u2/sync";

// === PINES Y HARDWARE ===
#define PIN_DHT        4
#define TIPO_DHT       DHT22
#define PIN_LDR        34
#define PIN_BUZZER     18
#define PIN_LED_ESTADO 2

const bool LDR_MODULO_INVERTIDO = false;
const bool BUZZER_ES_PASIVO = false;

// === OBJETOS Y ESTADO GLOBAL ===
DHT dht(PIN_DHT, TIPO_DHT);
WebServer server(80);
Preferences prefs;
WiFiClient tcpClient;

volatile float tempActual = 0.0;
volatile int humActual = 0;
volatile int lecturaAdcActual = 0;
volatile int luzPorcentajeActual = 0;
volatile bool dhtValido = false;
int fallosConsecutivosDht = 0;

volatile bool modoStandby = false;
volatile bool buzzerSilenciado = false;
volatile float tempLimite = 20.0;

unsigned long ultimaLecturaDhtMs = 0;
unsigned long ultimaLecturaLdrMs = 0;
unsigned long ultimoReintentoWifiMs = 0;
unsigned long ultimoIntentoMqttMs = 0;
unsigned long ultimoPingMqttMs = 0;
unsigned long ultimoCambioAlarmaMs = 0;
unsigned long ultimoParpadeoLedMs = 0;

bool buzzerSonando = false;
bool estadoLed = false;
bool mqttSuscrito = false;
bool usarBrokerSecundario = false;
bool ntpIniciado = false;
volatile bool forzarEnvioSync = true;
volatile bool pendienteGuardarFlash = false;
unsigned long contadorMensajes = 0;

// === UTILIDADES DE FLASH, HORA Y JSON ===

// Cargar configuración desde memoria flash
void cargarConfiguracionFlash() {
  prefs.begin("estacion_iot", true);
  tempLimite = prefs.getFloat("tempLimite", 20.0f);
  modoStandby = prefs.getBool("standby", false);
  buzzerSilenciado = prefs.getBool("buzzerMute", false);
  prefs.end();
}

// Guardar configuración en memoria flash
void guardarConfiguracionFlash() {
  prefs.begin("estacion_iot", false);
  prefs.putFloat("tempLimite", tempLimite);
  prefs.putBool("standby", modoStandby);
  prefs.putBool("buzzerMute", buzzerSilenciado);
  prefs.end();
}

// Obtener IP activa (STA o AP)
String obtenerIpActiva() {
  if (WiFi.status() == WL_CONNECTED) {
    return WiFi.localIP().toString();
  }
  return WiFi.softAPIP().toString();
}

// Obtener hora actual
String obtenerHoraActual() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 5)) {
    char buf[9];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return String(buf);
  }
  unsigned long seg = millis() / 1000;
  char buf[9];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
           (seg / 3600) % 24, (seg / 60) % 60, seg % 60);
  return String(buf);
}

// Buscar posición de un valor en JSON por clave
int buscarPosicionValorJson(const String& json, const char* clave) {
  String patron = String("\"") + clave + "\"";
  int pos = 0;
  while (true) {
    pos = json.indexOf(patron, pos);
    if (pos < 0) return -1;
    if (pos > 0 && json.charAt(pos - 1) == '\\') {
      pos += patron.length();
      continue;
    }
    int idx = pos + patron.length();
    while (idx < (int)json.length()) {
      char c = json.charAt(idx);
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        idx++;
      } else {
        break;
      }
    }
    if (idx < (int)json.length() && json.charAt(idx) == ':') {
      idx++;
      while (idx < (int)json.length()) {
        char c = json.charAt(idx);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
          idx++;
        } else {
          break;
        }
      }
      return idx;
    }
    pos += patron.length();
  }
}

// Extraer texto desde JSON
bool extraerJsonString(const String& json, const char* clave, String& salida) {
  int idx = buscarPosicionValorJson(json, clave);
  if (idx < 0 || idx >= (int)json.length() || json.charAt(idx) != '"') return false;
  idx++;
  String res = "";
  while (idx < (int)json.length()) {
    char c = json.charAt(idx);
    if (c == '\\' && idx + 1 < (int)json.length()) {
      res += json.charAt(idx + 1);
      idx += 2;
      continue;
    }
    if (c == '"') {
      salida = res;
      return true;
    }
    res += c;
    idx++;
  }
  return false;
}

// Extraer booleano desde JSON
bool extraerJsonBool(const String& json, const char* clave, bool& salida) {
  int idx = buscarPosicionValorJson(json, clave);
  if (idx < 0) return false;
  if (idx + 4 <= (int)json.length() && json.substring(idx, idx + 4) == "true") {
    salida = true;
    return true;
  }
  if (idx + 5 <= (int)json.length() && json.substring(idx, idx + 5) == "false") {
    salida = false;
    return true;
  }
  return false;
}

// Extraer número decimal desde JSON
bool extraerJsonFloat(const String& json, const char* clave, float& salida) {
  int idx = buscarPosicionValorJson(json, clave);
  if (idx < 0) return false;
  String num = "";
  bool tieneDigito = false;
  while (idx < (int)json.length()) {
    char c = json.charAt(idx);
    if (c >= '0' && c <= '9') {
      tieneDigito = true;
      num += c;
    } else if (c == '-' || c == '+' || c == '.') {
      num += c;
    } else {
      break;
    }
    idx++;
  }
  if (!tieneDigito) return false;
  salida = num.toFloat();
  return true;
}

// === SENSORES Y ACTUADORES ===

// Leer temperatura y humedad (DHT22)
void leerSensorDht22(bool forzar = false) {
  unsigned long ahora = millis();
  if (!forzar && (ahora - ultimaLecturaDhtMs < 2500)) return;
  ultimaLecturaDhtMs = ahora;

  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t) && !isnan(h) && t > -40.0f && t < 85.0f && h >= 0.0f && h <= 100.0f) {
    tempActual = roundf(t * 10.0f) / 10.0f;
    humActual = constrain((int)roundf(h), 0, 100);
    dhtValido = true;
    fallosConsecutivosDht = 0;
  } else {
    fallosConsecutivosDht++;
    if (fallosConsecutivosDht == 3 || (fallosConsecutivosDht % 12 == 0)) {
      Serial.println("[AVISO DHT22] Sin lectura en GPIO 4. Verifica VCC 3.3V, GND, DATA y pull-up 10k.");
    }
    if (fallosConsecutivosDht >= 5) {
      dhtValido = false;
    }
  }
}

// Leer luminosidad (LDR)
void leerSensorLdr(bool forzar = false) {
  unsigned long ahora = millis();
  if (!forzar && (ahora - ultimaLecturaLdrMs < 350)) return;
  ultimaLecturaLdrMs = ahora;

  long suma = 0;
  for (int i = 0; i < 16; i++) {
    suma += analogRead(PIN_LDR);
    delayMicroseconds(60);
  }
  int adc = constrain((int)(suma / 16), 0, 4095);
  lecturaAdcActual = adc;

  int pct = LDR_MODULO_INVERTIDO
                ? map(adc, 4095, 0, 0, 100)
                : map(adc, 0, 4095, 0, 100);
  luzPorcentajeActual = constrain(pct, 0, 100);
}

// Apagar buzzer
void apagarBuzzerFisico() {
  if (BUZZER_ES_PASIVO) {
    noTone(PIN_BUZZER);
  }
  digitalWrite(PIN_BUZZER, LOW);
  buzzerSonando = false;
}

// Encender buzzer
void encenderBuzzerFisico() {
  if (BUZZER_ES_PASIVO) {
    tone(PIN_BUZZER, 2000);
  } else {
    digitalWrite(PIN_BUZZER, HIGH);
  }
  buzzerSonando = true;
}

// Actualizar buzzer y LED de estado
void actualizarActuadores() {
  unsigned long ahora = millis();
  bool alertaActiva = dhtValido && (tempActual > tempLimite) && !buzzerSilenciado && !modoStandby;

  if (!alertaActiva) {
    if (buzzerSonando) {
      apagarBuzzerFisico();
    }
  } else {
    if (!buzzerSonando && (ahora - ultimoCambioAlarmaMs >= 650)) {
      encenderBuzzerFisico();
      ultimoCambioAlarmaMs = ahora;
    } else if (buzzerSonando && (ahora - ultimoCambioAlarmaMs >= 350)) {
      apagarBuzzerFisico();
      ultimoCambioAlarmaMs = ahora;
    }
  }

  if (alertaActiva) {
    if (ahora - ultimoParpadeoLedMs >= 150) {
      ultimoParpadeoLedMs = ahora;
      estadoLed = !estadoLed;
      digitalWrite(PIN_LED_ESTADO, estadoLed ? HIGH : LOW);
    }
  } else if (modoStandby) {
    if (ahora - ultimoParpadeoLedMs >= 1200) {
      ultimoParpadeoLedMs = ahora;
      estadoLed = !estadoLed;
      digitalWrite(PIN_LED_ESTADO, estadoLed ? HIGH : LOW);
    }
  } else if (WiFi.status() != WL_CONNECTED) {
    if (ahora - ultimoParpadeoLedMs >= 500) {
      ultimoParpadeoLedMs = ahora;
      estadoLed = !estadoLed;
      digitalWrite(PIN_LED_ESTADO, estadoLed ? HIGH : LOW);
    }
  } else {
    if (!estadoLed) {
      estadoLed = true;
      digitalWrite(PIN_LED_ESTADO, HIGH);
    }
  }
}

// === SERVIDOR WEB HTTP LOCAL ===

// Construir JSON de telemetría
String construirJsonTelemetria(bool incluirCamposSync) {
  String ipStr = obtenerIpActiva();
  float t = tempActual;
  int h = humActual;
  int l = luzPorcentajeActual;
  int a = lecturaAdcActual;
  float lim = tempLimite;
  bool sb = modoStandby;
  bool bm = buzzerSilenciado;
  bool dok = dhtValido;

  String json = "{";
  if (incluirCamposSync) {
    contadorMensajes++;
    String uid = "esp32_" + String((uint32_t)ESP.getEfuseMac(), HEX) + "_" + String(millis()) + "_" + String(contadorMensajes);
    String detalle;
    if (!dok) {
      detalle = "⚠️ Revisar sensor DHT22 en GPIO 4 (" + ipStr + ")";
    } else if (sb) {
      detalle = "⏸️ ESP32 Físico en Standby (" + ipStr + ")";
    } else if (t > lim) {
      detalle = "🔥 Alerta Térmica ESP32 (>" + String(lim, 1) + "°C)";
    } else {
      detalle = "📡 Lectura ESP32 Físico (" + ipStr + ")";
    }
    json += "\"uid\":\"" + uid + "\",";
    json += "\"tipo\":\"lectura\",";
    json += "\"hora\":\"" + obtenerHoraActual() + "\",";
    json += "\"detalle\":\"" + detalle + "\",";
  }
  json += "\"temp\":" + String(t, 1) + ",";
  json += "\"hum\":" + String(h) + ",";
  json += "\"ldr\":" + String(l) + ",";
  json += "\"adc\":" + String(a) + ",";
  json += "\"tempLimite\":" + String(lim, 1) + ",";
  json += "\"standby\":" + String(sb ? "true" : "false") + ",";
  json += "\"buzzerMute\":" + String(bm ? "true" : "false") + ",";
  json += "\"dhtOk\":" + String(dok ? "true" : "false") + ",";
  json += "\"esEsp32\":true,";
  json += "\"origen\":\"ESP32 Físico (" + ipStr + ")\",";
  json += "\"ip\":\"" + ipStr + "\"";
  json += "}";
  return json;
}

// Enviar cabeceras CORS
void enviarCabecerasCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
}

// Ruta HTTP "/"
void handleRaiz() {
  String ip = obtenerIpActiva();
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta http-equiv='refresh' content='3'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'><title>Estación IoT ESP32</title>";
  html += "<style>body{font-family:sans-serif;max-width:460px;margin:24px auto;padding:16px;background:#f1f4f9;}";
  html += ".card{background:#fff;border-radius:14px;padding:18px;box-shadow:0 2px 8px rgba(0,0,0,.08);}h2{color:#1a237e;margin-top:0;}</style></head><body>";
  html += "<div class='card'><h2>Estación Meteorológica ESP32</h2>";
  html += "<p><b>Temperatura:</b> " + String(tempActual, 1) + " &deg;C" + (dhtValido ? "" : " (sin lectura DHT22)") + "</p>";
  html += "<p><b>Humedad:</b> " + String(humActual) + " %</p>";
  html += "<p><b>Luminosidad:</b> " + String(luzPorcentajeActual) + " % (ADC: " + String(lecturaAdcActual) + " / 4095)</p>";
  html += "<p><b>Límite Alarma:</b> " + String(tempLimite, 1) + " &deg;C</p>";
  html += "<p><b>Standby:</b> " + String(modoStandby ? "ON" : "OFF") + " | <b>Buzzer:</b> " + String(buzzerSilenciado ? "MUTE" : "ACTIVO") + "</p>";
  html += "<p><b>IP Activa:</b> " + ip + " | <b>AP:</b> " + String(ap_ssid) + " (" + WiFi.softAPIP().toString() + ")</p>";
  html += "<p><a href='/telemetria'>Ver JSON /telemetria</a></p></div></body></html>";
  enviarCabecerasCors();
  server.send(200, "text/html; charset=utf-8", html);
}

// Ruta HTTP "/telemetria"
void handleTelemetria() {
  leerSensorLdr(true);
  actualizarActuadores();
  enviarCabecerasCors();
  server.send(200, "application/json; charset=utf-8", construirJsonTelemetria(false));
}

// Ruta HTTP "/control"
void handleControl() {
  bool cambio = false;

  if (server.hasArg("standby")) {
    String valor = server.arg("standby");
    valor.toUpperCase();
    bool nuevoStandby = (valor == "ON" || valor == "TRUE" || valor == "1");
    if (nuevoStandby != modoStandby) {
      modoStandby = nuevoStandby;
      cambio = true;
    }
  }

  if (server.hasArg("buzzer")) {
    String valor = server.arg("buzzer");
    valor.toUpperCase();
    if (valor == "MUTE" || valor == "OFF" || valor == "TRUE" || valor == "1") {
      if (!buzzerSilenciado) {
        buzzerSilenciado = true;
        cambio = true;
      }
    } else if (valor == "ON" || valor == "ACTIVE" || valor == "FALSE" || valor == "0") {
      if (buzzerSilenciado) {
        buzzerSilenciado = false;
        cambio = true;
      }
    }
  }

  if (server.hasArg("tempLimite") || server.hasArg("limite")) {
    String valor = server.hasArg("tempLimite") ? server.arg("tempLimite") : server.arg("limite");
    float nuevoLimite = roundf(valor.toFloat() * 10.0f) / 10.0f;
    if (nuevoLimite >= -20.0f && nuevoLimite <= 80.0f && fabsf(nuevoLimite - tempLimite) >= 0.05f) {
      tempLimite = nuevoLimite;
      cambio = true;
    }
  }

  if (cambio) {
    guardarConfiguracionFlash();
    forzarEnvioSync = true;
    Serial.printf("[APP -> ESP32 HTTP] TempLimite: %.1f C | Standby: %s | Buzzer: %s\n",
                  tempLimite,
                  modoStandby ? "ON" : "OFF",
                  buzzerSilenciado ? "MUTE" : "ACTIVO");
  }

  actualizarActuadores();
  enviarCabecerasCors();
  server.send(200, "application/json; charset=utf-8", "{\"status\":\"OK\"," + construirJsonTelemetria(false).substring(1));
}

// === CANAL MQTT EN CORE 0 ===

// Codificar longitud restante de paquete MQTT
int escribirLongitudMqtt(uint8_t* buf, size_t len) {
  int idx = 0;
  do {
    uint8_t encoded = len % 128;
    len /= 128;
    if (len > 0) encoded |= 0x80;
    buf[idx++] = encoded;
  } while (len > 0 && idx < 4);
  return idx;
}

// Leer un byte desde la conexión MQTT
bool leerByteMqtt(uint8_t& outByte, unsigned long timeoutMs = 250) {
  unsigned long inicio = millis();
  while (!tcpClient.available()) {
    if (!tcpClient.connected() || (millis() - inicio >= timeoutMs)) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  int b = tcpClient.read();
  if (b < 0) return false;
  outByte = (uint8_t)b;
  return true;
}

// Enviar paquete CONNECT al broker MQTT
bool enviarPaqueteConnectMqtt(const char* clientId) {
  size_t idLen = strlen(clientId);
  size_t remLen = 10 + 2 + idLen;
  uint8_t header[5];
  header[0] = 0x10;
  int lenBytes = escribirLongitudMqtt(&header[1], remLen);

  uint8_t varHeader[10] = {
    0x00, 0x04, 'M', 'Q', 'T', 'T',
    0x04,
    0x02,
    0x00, 0x3C
  };

  tcpClient.write(header, 1 + lenBytes);
  tcpClient.write(varHeader, 10);
  uint8_t idLenBytes[2] = { (uint8_t)(idLen >> 8), (uint8_t)(idLen & 0xFF) };
  tcpClient.write(idLenBytes, 2);
  tcpClient.write((const uint8_t*)clientId, idLen);
  tcpClient.flush();

  uint8_t resp[4];
  for (int i = 0; i < 4; i++) {
    if (!leerByteMqtt(resp[i], 2000)) return false;
  }
  return ((resp[0] & 0xF0) == 0x20 && resp[3] == 0x00);
}

// Suscribirse a un topic MQTT
void suscribirTopicMqtt(const char* topic) {
  size_t tLen = strlen(topic);
  size_t remLen = 2 + 2 + tLen + 1;
  uint8_t header[5];
  header[0] = 0x82;
  int lenBytes = escribirLongitudMqtt(&header[1], remLen);

  tcpClient.write(header, 1 + lenBytes);
  uint8_t pktId[2] = { 0x00, 0x01 };
  tcpClient.write(pktId, 2);
  uint8_t tLenBytes[2] = { (uint8_t)(tLen >> 8), (uint8_t)(tLen & 0xFF) };
  tcpClient.write(tLenBytes, 2);
  tcpClient.write((const uint8_t*)topic, tLen);
  uint8_t qos = 0x00;
  tcpClient.write(&qos, 1);
  tcpClient.flush();
}

// Publicar mensaje en topic MQTT
bool publicarMqtt(const char* topic, const String& payload) {
  if (!tcpClient.connected() || !mqttSuscrito) return false;

  size_t tLen = strlen(topic);
  size_t pLen = payload.length();
  size_t remLen = 2 + tLen + pLen;

  uint8_t header[5];
  header[0] = 0x30;
  int lenBytes = escribirLongitudMqtt(&header[1], remLen);

  tcpClient.write(header, 1 + lenBytes);
  uint8_t tLenBytes[2] = { (uint8_t)(tLen >> 8), (uint8_t)(tLen & 0xFF) };
  tcpClient.write(tLenBytes, 2);
  tcpClient.write((const uint8_t*)topic, tLen);
  tcpClient.write((const uint8_t*)payload.c_str(), pLen);
  tcpClient.flush();
  return true;
}

// Procesar comandos JSON recibidos desde la app
void procesarMensajeJsonApp(const String& jsonStr) {
  if (!jsonStr.startsWith("{")) return;

  String origen = "";
  extraerJsonString(jsonStr, "origen", origen);
  bool esEsp32 = false;
  extraerJsonBool(jsonStr, "esEsp32", esEsp32);
  if (esEsp32 || origen.startsWith("ESP32")) return;

  String tipo = "";
  extraerJsonString(jsonStr, "tipo", tipo);

  if (tipo == "ping_esp32" || tipo == "solicitar_lectura") {
    forzarEnvioSync = true;
    return;
  }

  if (tipo == "usuario" || tipo == "usuario_actualizar" ||
      tipo == "usuario_eliminar" || tipo == "limpiar") {
    return;
  }

  bool huboCambio = false;

  float nuevoLim = 0.0f;
  if (extraerJsonFloat(jsonStr, "tempLimite", nuevoLim)) {
    nuevoLim = roundf(nuevoLim * 10.0f) / 10.0f;
    if (nuevoLim >= -20.0f && nuevoLim <= 80.0f && fabsf(nuevoLim - tempLimite) >= 0.05f) {
      tempLimite = nuevoLim;
      huboCambio = true;
    }
  }

  bool nuevoStandby = false;
  if (extraerJsonBool(jsonStr, "standby", nuevoStandby) && nuevoStandby != modoStandby) {
    modoStandby = nuevoStandby;
    huboCambio = true;
  }

  bool nuevoMute = false;
  if (extraerJsonBool(jsonStr, "buzzerMute", nuevoMute) && nuevoMute != buzzerSilenciado) {
    buzzerSilenciado = nuevoMute;
    huboCambio = true;
  }

  if (huboCambio) {
    pendienteGuardarFlash = true;
    forzarEnvioSync = true;
    Serial.printf("[APP -> ESP32 MQTT] TempLimite: %.1f C | Standby: %s | Buzzer: %s\n",
                  tempLimite,
                  modoStandby ? "ON" : "OFF",
                  buzzerSilenciado ? "MUTE" : "ACTIVO");
  }
}

// Mantener conexión con el broker MQTT
void asegurarConexionMqttCore0() {
  if (!HABILITAR_MQTT_NUBE || WiFi.status() != WL_CONNECTED) return;
  if (tcpClient.connected() && mqttSuscrito) return;

  unsigned long ahora = millis();
  if (ahora - ultimoIntentoMqttMs < 8000) return;
  ultimoIntentoMqttMs = ahora;

  tcpClient.stop();
  mqttSuscrito = false;

  const char* broker = usarBrokerSecundario ? MQTT_BROKER_SECUNDARIO : MQTT_BROKER_PRIMARIO;
  IPAddress ipBroker;
  if (!WiFi.hostByName(broker, ipBroker)) {
    usarBrokerSecundario = !usarBrokerSecundario;
    return;
  }

  tcpClient.setTimeout(1500);
  if (!tcpClient.connect(ipBroker, MQTT_PORT, 1500)) {
    usarBrokerSecundario = !usarBrokerSecundario;
    return;
  }

  String clientId = "esp32_real_" + String((uint32_t)ESP.getEfuseMac(), HEX) + "_" + String(millis() % 10000);
  if (enviarPaqueteConnectMqtt(clientId.c_str())) {
    suscribirTopicMqtt(MQTT_TOPIC);
    mqttSuscrito = true;
    ultimoPingMqttMs = ahora;
    forzarEnvioSync = true;
    Serial.printf("[MQTT OK] Enlazado a %s:%d (Topic: %s)\n", broker, MQTT_PORT, MQTT_TOPIC);
  } else {
    tcpClient.stop();
    usarBrokerSecundario = !usarBrokerSecundario;
  }
}

// Leer y procesar paquetes entrantes MQTT
void escucharComandosMqttCore0() {
  if (!tcpClient.connected()) {
    mqttSuscrito = false;
    return;
  }

  while (tcpClient.available() > 0) {
    uint8_t header = 0;
    if (!leerByteMqtt(header, 100)) return;
    uint8_t packetType = header & 0xF0;

    size_t multiplier = 1;
    size_t remLen = 0;
    uint8_t encodedByte = 0;
    int lenCount = 0;
    do {
      if (!leerByteMqtt(encodedByte, 250)) {
        tcpClient.stop();
        mqttSuscrito = false;
        return;
      }
      remLen += (encodedByte & 127) * multiplier;
      multiplier *= 128;
      lenCount++;
      if (lenCount > 4) {
        tcpClient.stop();
        mqttSuscrito = false;
        return;
      }
    } while ((encodedByte & 128) != 0);

    if (remLen > 2048) {
      for (size_t i = 0; i < remLen; i++) {
        uint8_t dummy;
        if (!leerByteMqtt(dummy, 200)) break;
      }
      continue;
    }

    if (packetType == 0x30 && remLen >= 2) {
      uint8_t tLenH = 0, tLenL = 0;
      if (!leerByteMqtt(tLenH, 250) || !leerByteMqtt(tLenL, 250)) break;
      uint16_t topicLen = ((uint16_t)tLenH << 8) | tLenL;

      for (uint16_t i = 0; i < topicLen; i++) {
        uint8_t dummy;
        if (!leerByteMqtt(dummy, 200)) break;
      }

      size_t payloadLen = (remLen > (size_t)(2 + topicLen)) ? (remLen - 2 - topicLen) : 0;
      if ((header & 0x06) != 0 && payloadLen >= 2) {
        uint8_t dummy1, dummy2;
        leerByteMqtt(dummy1, 200);
        leerByteMqtt(dummy2, 200);
        payloadLen -= 2;
      }

      String msg = "";
      msg.reserve(payloadLen + 1);
      for (size_t i = 0; i < payloadLen; i++) {
        uint8_t ch = 0;
        if (!leerByteMqtt(ch, 250)) break;
        msg += (char)ch;
      }
      msg.trim();
      if (msg.length() > 0) {
        procesarMensajeJsonApp(msg);
      }
    } else {
      for (size_t i = 0; i < remLen; i++) {
        uint8_t dummy;
        if (!leerByteMqtt(dummy, 200)) break;
      }
    }
  }

  unsigned long ahora = millis();
  if (ahora - ultimoPingMqttMs >= 20000) {
    ultimoPingMqttMs = ahora;
    uint8_t pingReq[2] = { 0xC0, 0x00 };
    tcpClient.write(pingReq, 2);
    tcpClient.flush();
  }
}

// Tarea en Core 0 para sincronización MQTT
void tareaSincronizacionMqtt(void* parameter) {
  float ultimaTempPublicada = -999.0f;
  int ultimaHumPublicada = -1;
  int ultimaLuzPublicada = -1;
  unsigned long ultimoEnvioMs = 0;

  for (;;) {
    if (HABILITAR_MQTT_NUBE && WiFi.status() == WL_CONNECTED) {
      asegurarConexionMqttCore0();
      escucharComandosMqttCore0();

      unsigned long ahora = millis();
      bool cambioSensores = fabsf(tempActual - ultimaTempPublicada) >= 0.4f ||
                            abs(humActual - ultimaHumPublicada) >= 2 ||
                            abs(luzPorcentajeActual - ultimaLuzPublicada) >= 4;

      if (mqttSuscrito &&
          (forzarEnvioSync || (ahora - ultimoEnvioMs >= 4000) || (cambioSensores && (ahora - ultimoEnvioMs >= 1500)))) {
        String payload = construirJsonTelemetria(true);
        if (publicarMqtt(MQTT_TOPIC, payload)) {
          ultimaTempPublicada = tempActual;
          ultimaHumPublicada = humActual;
          ultimaLuzPublicada = luzPorcentajeActual;
          forzarEnvioSync = false;
          ultimoEnvioMs = millis();
          Serial.printf("[ESP32 -> APP] Temp: %.1f C | Hum: %d%% | Luz: %d%% | Limite: %.1f C | Standby: %s\n",
                        tempActual, humActual, luzPorcentajeActual, tempLimite, modoStandby ? "ON" : "OFF");
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// === SETUP Y LOOP PRINCIPAL ===

// Configuración inicial (setup)
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_LED_ESTADO, OUTPUT);
  digitalWrite(PIN_LED_ESTADO, LOW);

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_LDR, ADC_11db);

  cargarConfiguracionFlash();
  dht.begin();

  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.softAP(ap_ssid, ap_password);

  Serial.println("\n==========================================");
  Serial.println("  ESTACIÓN METEOROLÓGICA IOT (ESP32 REAL)");
  Serial.print("Red AP Directa activa: ");
  Serial.println(ap_ssid);
  Serial.print("IP AP del ESP32:       ");
  Serial.println(WiFi.softAPIP());

  if (strlen(ssid) > 0 && strcmp(ssid, "TU_RED_WIFI") != 0) {
    WiFi.begin(ssid, password);
    Serial.printf("Conectando a Wi-Fi '%s'", ssid);
    unsigned long inicio = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - inicio < 10000)) {
      delay(350);
      Serial.print(".");
      digitalWrite(PIN_LED_ESTADO, !digitalRead(PIN_LED_ESTADO));
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n[OK] Wi-Fi conectado exitosamente!");
      Serial.print("IP LAN del ESP32:      ");
      Serial.println(WiFi.localIP());
      Serial.printf("Señal Wi-Fi (RSSI):    %d dBm\n", WiFi.RSSI());
      configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
      ntpIniciado = true;
    } else {
      Serial.println("\n[AVISO] Router aún no responde; operando en AP 192.168.4.1 y reintentando STA en segundo plano.");
    }
  } else {
    Serial.println("[TIP] Cambia 'ssid' y 'password' para usar tu Wi-Fi/Hotspot, o conéctate a 'EstacionIoT-ESP32' (192.168.4.1).");
  }
  Serial.println("==========================================");

  if (MDNS.begin("estacion-iot")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("[mDNS] http://estacion-iot.local");
  }

  delay(1500);
  leerSensorDht22(true);
  leerSensorLdr(true);
  actualizarActuadores();

  server.on("/", HTTP_GET, handleRaiz);
  server.on("/telemetria", HTTP_GET, handleTelemetria);
  server.on("/control", HTTP_GET, handleControl);
  server.begin();
  Serial.printf("[HTTP OK] Telemetría lista en http://%s/telemetria\n", obtenerIpActiva().c_str());

  xTaskCreatePinnedToCore(
    tareaSincronizacionMqtt,
    "SyncMqttTask",
    8192,
    NULL,
    1,
    NULL,
    0
  );
}

// Bucle principal (loop)
void loop() {
  server.handleClient();

  if (pendienteGuardarFlash) {
    pendienteGuardarFlash = false;
    guardarConfiguracionFlash();
  }

  leerSensorDht22(false);
  leerSensorLdr(false);
  actualizarActuadores();

  unsigned long ahora = millis();
  if (WiFi.status() == WL_CONNECTED) {
    if (!ntpIniciado) {
      configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
      ntpIniciado = true;
    }
  } else if (strlen(ssid) > 0 && strcmp(ssid, "TU_RED_WIFI") != 0) {
    if (ahora - ultimoReintentoWifiMs >= 15000) {
      ultimoReintentoWifiMs = ahora;
      WiFi.reconnect();
    }
  }
}
