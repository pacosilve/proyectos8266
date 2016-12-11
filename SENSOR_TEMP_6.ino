/**
Sensor de Temperatura/Termostato Wifi
SENSOR_TEMP.ino
Purpose: Termostato conectado. Lee la temperatura y la comunica a un servidor MQTT.
Muestra información por pantalla (OLED 128x64), y se controla mediante un encoder.

En funcionamiento normal, apaga la pantalla y entra en modo deepSleep, despertando mediante
un reset tras un periodo configurable, para hacer una lectura y enviar los datos al servidor.
De este modo, se maximiza la duración de la batería.

Para salir del funcionamiento normal, es necesario pulsar RESET, pasando a modo interactivo.
En este modo es posible visualizar la temperatura, fijar la temperatura del termostato,
revisar los datos de configuración, y restablecer la configuración a sus valores por defecto.

Si no consigue conectarse a la red en el inicio, no hay datos de configuración,
o se pierde la conectividad, el dispositivo entra en modo AccessPoint(AP), habilitándose un
servidor web al que conectarse para permitir la configuración del dispositivo.

Guarda la configuración en EEPROM.

@author Manuel Caballero
@version 1.0 08/05/2016
*/

#include <ESP8266WiFi.h>
#include <SPI.h>
#include <Wire.h>
#include <ESP8266WebServer.h>
#include <WiFiClient.h>

#include <OneWire.h>                 //Se importan las librerías
#include <DallasTemperature.h>

#include <PubSubClient.h>
#include <EEPROM.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Encoder.h>
#include <SimpleTimer.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans24pt7b.h>
#include <TimeLib.h>
#include <NtpClientLib.h>

//#include <DHT.h>


/*Constantes*/
/*Valores por defecto para AP que se crea si no consigue conectarse.
La contraseña debe estar vacía o tener un tamaño minimo de 8 caracteres
de lo contrario, no se configura correctamente el AP, y se queda con un
SSID del tipo 8266XXXX y sin pwd*/
#define DEFAULT_ID  "sensor_1"
#define DEFAULT_AP_SSID  "ESP8266_TEMP"
#define DEFAULT_AP_PASS  "12345678"
// Una vez levantado el AP, se resetea cada 10 min para probar la conexion automaticamente 600.000 milisegundos
#define DEFAULT_TIME_TO_RESET_AP = 600000;
// Datos por defecto servidor mqtt
#define DEFAULT_MQTT_SERVER  "192.168.1.100"
#define DEFAULT_MQTT_PORT  1883
#define DEFAULT_MQTT_DEVICE_ID "ESP8266Client"
#define DEFAULT_MQTT_USER ""
#define DEFAULT_MQTT_PASS ""
#define DEFAULT_MQTT_HUMIDITY_PATH "/humedad/get"
#define DEFAULT_MQTT_TEMPERATURE_PATH "/temperatura/get"
#define DEFAULT_MQTT_TEMPERATURE_THRESHOLD_PATH "/temperatura/set"
#define DEFAULT_MQTT_VOLTAJE_PATH "/bateria/get"
#define DEFAULT_MQTT_PUBLISH_INTERVAL 300000
#define DEFAULT_TEMP_OBJETIVO 21
#define DEFAULT_TIEMPO_MODO_AHORRO 10000

// Pines y constantes HW
#define OLED_RESET_PIN 0
#define OLED_POWER_PIN 15
#define I2C_SDA_PIN 5
#define I2C_SCL_PIN 4
#define ROTARY_SWITCH_PIN 14
#define ROTARY_DT_PIN 13
#define ROTARY_CK_PIN 12

#define TEMP_PIN 2
//#define DHTPIN 2
//#define DHTTYPE DHT22


#define LOGO16_GLCD_HEIGHT 64 
#define LOGO16_GLCD_WIDTH  128 

ADC_MODE(ADC_VCC);

// Estructura con la configuración
typedef struct {
  char id[32];
  char net_ssid[32];
  char net_pass[48];
  boolean net_manualConfig;
  uint32_t net_IP;
  uint32_t net_gateWay;
  uint32_t net_mask;
  uint32_t net_dns1;
  uint32_t net_dns2;
  boolean ap_activate;
  char ap_ssid[32];
  char ap_pass[48];
  ulong ap_timeout;
  char mqtt_server[128];
  uint16_t mqtt_port;
  char mqtt_device_id[32];
  char mqtt_user[32];
  char mqtt_pass[48];
  ulong mqtt_temper_intervalo_muestreo;
  char mqtt_humedad_ruta_publicacion[64];
  char mqtt_temper_ruta_publicacion[64];
  char mqtt_umbral_ruta_publicacion[64];
  float temp_objetivo;
} Configuracion;

enum ModosFunc {
  MODO_AHORRO,
  MODO_INTERACTIVO,
  MODO_AP
};

// Objeto controlador del encoder
Encoder encoder(ROTARY_DT_PIN, ROTARY_CK_PIN);

// Objeto para manejar la pantalla OLED
Adafruit_SSD1306 display(OLED_RESET_PIN);

// Objeto para manejar el servidor WEB
ESP8266WebServer server(80);

// Objeto temporizador, para lanzar funciones de forma programada
SimpleTimer timer;

// Objeto con el sensor de temperatura y humedad
//DHT dht(DHTPIN, DHTTYPE, 15);
OneWire ourWire(TEMP_PIN);                //Se establece el pin declarado como bus para la comunicación OneWire
DallasTemperature sensors(&ourWire); //Se llama a la librería DallasTemperature


// Cliente de red 
WiFiClient espClient;

// Cliente MQTT
PubSubClient client(espClient);

const char * estados_wifi[] = {
  "Inactivo",
  "Sin redes",
  "Busq.compl",
  "Conectado",
  "Error con",
  "Con.perdida",
  "Desconectado" };

const char * menus[] = {
  "Temperatura",
  "Temp. Config.",
  "Red",
  "Configuracion",
  "Restaurar"
};

const uint8_t numeroMenus = sizeof(menus) / sizeof(char*);
uint8_t menuActual = sizeof(menus)-1;

uint8_t pulsadorVal=1;
uint8_t pulsadorValAnterior=1;

int     posicion_anterior = -1;
boolean menuActivo = true;
boolean cambio = false;

boolean modoEdicionPantalla = false;

boolean webServerOn = false;
float   humedad_actual = 0;
float   temp_actual = 0;
//float temp_objetivo = DEFAULT_TEMP_OBJETIVO;
boolean mqtt_connected = false;
uint8_t numero_reintentos = 0;

ModosFunc modo = MODO_INTERACTIVO;

Configuracion config;
// este temporizador se usará mientras se esté en modo interactivo detectar cuando se ha de pasar al modo normal
uint32_t timerActividad;
// este temporizador se usará mientras se esté en modo interactivo para lanzar lecturas del sensor
uint32_t timerMuestreo;

// Tensión de la Batería
#define VCC_MAX  3.30
#define VCC_MIN  2.3

uint8_t modo_arranque = 0;

double  vcc= 0;
double  vcc_perc = 100;

/*------------------------------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------------------------------*/
/*   setup()                                                                                            */
/*------------------------------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------------------------------*/

void setup() {
  static WiFiEventHandler e1, e2;
  rst_info *rsti;

  Serial.begin(115200);

  rsti = ESP.getResetInfoPtr();
  Serial.println("\r\nStart...");
  Serial.println(String("ResetInfo.reason = ") + rsti->reason);

  modo_arranque = rsti->reason;  
 
  // Define la funcion del evento de la wifi para conectar por NTP
  e1 = WiFi.onStationModeGotIP(onSTAGotIP);

   /* Gestiona el evento de sincronización NTP */
  NTP.onNTPSyncEvent([](NTPSyncEvent_t ntpEvent) {
    if (ntpEvent) {
      Serial.print("Time Sync error: ");
      if (ntpEvent == noResponse)
        Serial.println("NTP server not reachable");
      else if (ntpEvent == invalidAddress)
        Serial.println("Invalid NTP server address");
    }
    else {
      Serial.print("Got NTP time: ");
      Serial.println(NTP.getTimeDateString(NTP.getLastNTPSync()));
    }
  });
  
  // Lee el voltaje de la batería
  getBattery();

  // Inicializa el sensor de temperatura
  sensors.begin();   
   
  // Inicializa pines encoder
  pinMode(ROTARY_SWITCH_PIN, INPUT_PULLUP);
  pinMode(ROTARY_DT_PIN, INPUT); // estos pines ya llevan sus propias resistencias de pull-up
  pinMode(ROTARY_CK_PIN, INPUT); // estos pines ya llevan sus propias resistencias de pull-up

  // Inicializamos los pines I2C (están al revés)
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  // Configuramos Wifi en modo cliente
  WiFi.mode(WIFI_STA);
  
  // Inicializa variables    
  resetConfig();

  // Si el modo de arranque proviene de un DeepSleep entonces arrancamos en modo AHORRO. 
  if(modo_arranque == 5) modo = MODO_AHORRO;

  if (modo != MODO_AHORRO) { 
    initPantalla();
    leerDatos();  // Lee Temperatura
    mostrarTemperatura();
  }
  
  // Lee EEPROM, carga datos de configuración y configura Wifi cliente
  if (cargarDatosEEPROM()) {
    Serial.println("Datos EEPROM cargados, con info de SSID");    
    // Probamos la conexión 
    if (!conectarWifi()) {
      modo = MODO_AP;
    }
    else {
      // Hay conectividad
      // Configuramos el servidor mqtt, y configuramos las suscripciones 
      conectarMQTT();
    }
  }
  else {
    // El SSID leido de EEPROM está vacio. Activamos AP
    Serial.println("Datos EEPROM cargados, sin info de SSID");
    modo = MODO_AP;
  }
  
  if (modo == MODO_AP) {
    strcpy(config.ap_pass, DEFAULT_AP_PASS);
    strcpy(config.ap_ssid, DEFAULT_AP_SSID);
    initPantalla();
    setupAP();
    //Programamos un reset dentro de un tiempo, por si el problema de red era temporal
    timer.setTimeout(config.ap_timeout, doResetAP);

  }

  if (modo != MODO_AHORRO) {
    // Levantamos el servidor web
    setupServer();
    // Todo: Encender la pantalla, al principio del todo, nada más detectar que no procede de un DeepSleep
  }
  
  if (modo == MODO_INTERACTIVO) {
    //En modo interactivo, configuramos temporizadores para monitorizar la actividad, y para lanzar las lecturas
    timerActividad = timer.setTimeout(DEFAULT_TIEMPO_MODO_AHORRO, modoAhorro);
    timer.setInterval(5000, leerEnviarDatos); // Leemos la temperatura cada 5 seg en modo interactivo.
  }


}

/*------------------------------------------------------------------------------------------------------*/
void initPantalla(){
  //Inicializamos el pin de alimentacion de la pantalla
  pinMode(OLED_POWER_PIN, OUTPUT);
  digitalWrite(OLED_POWER_PIN, HIGH);
  // Inicializa la pantalla 
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.display();
}
/*------------------------------------------------------------------------------------------------------*/
/*
 *     conectarWifi(void)
 */
/*------------------------------------------------------------------------------------------------------*/

boolean conectarWifi(void) {
  Serial.print("Conectando a ");
  Serial.print(config.net_ssid);
  mostrarStatusInit("Connectando a Wifi..."); 
  Serial.print("/");
  Serial.println(config.net_pass);

  WiFi.begin(config.net_ssid, config.net_pass);
  if (config.net_manualConfig == true) {
    Serial.println("Configuracion manual");
    WiFi.config(
      IPAddress(config.net_IP),
      IPAddress(config.net_gateWay),
      IPAddress(config.net_mask),
      IPAddress(config.net_dns1),
      IPAddress(config.net_dns2));
  }
  
  WiFi.setAutoConnect(true);
  
  int c = 0;
  Serial.println("Esperando conexion Wifi");
  while (c < 20) {
    Serial.print(".");
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("");
      Serial.println("WiFi conectado");
      Serial.println("IP: ");
      Serial.println(WiFi.localIP());
      return true;
    }
    delay(500);
    c++;
  }
  Serial.print("No conectado: ");
  if (WiFi.status() <= 6) {
    Serial.println(estados_wifi[WiFi.status()]);
  }
  else {
    Serial.println(WiFi.status());
  }
  return false;
}


void onSTAGotIP(WiFiEventStationModeGotIP ipInfo) {
  Serial.printf("Got IP: %s\r\n", ipInfo.ip.toString().c_str());
  NTP.begin("pool.ntp.org", 1, true);
  NTP.setInterval(63);
}


/*------------------------------------------------------------------------------------------------------*/
/*
 *     setupAP() 
 */
/*------------------------------------------------------------------------------------------------------*/

void setupAP() {
  WiFi.mode(WIFI_AP_STA);
  Serial.println();
  Serial.print("Iniciando punto de acceso ");
  mostrarStatusInit("Iniciando punto de acceso ");
  Serial.println(config.ap_ssid);

  /* Si el tamaño de la contraseña es menor de 8 caracteres, no la ponemos para evitar el bug */
  if (strlen(config.ap_pass) >= 8) {
    WiFi.softAP(config.ap_ssid, config.ap_pass); // Wifi.softAP(ssid, password)
  }
  else {
    WiFi.softAP(config.ap_ssid); // Wifi.softAP(ssid)
  }

  IPAddress myIP = WiFi.softAPIP();
  Serial.print("IP AP: ");
  Serial.println(myIP);
  // Clear the buffer.
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.println("MODO AP");
  display.setTextSize(0);
  display.print("IP AP: ");
  display.println(myIP);
  display.print("SSID: ");
  display.println(config.ap_ssid); 
  display.print("Temp: ");
  display.println(temp_actual); 

  display.display();

}



boolean cargarDatosEEPROM() {
  EEPROM.begin(sizeof(Configuracion));
  delay(10);
  Serial.println();
  Serial.println();
  Serial.println("Inicializando");
  EEPROM.get(0, config);
  EEPROM.end();
  delay(10);

  if (strlen(config.ap_ssid) == 0) {
    Serial.print("No hay datos para AP_SSID, se usara por defecto ");
    Serial.print(DEFAULT_AP_SSID);
    Serial.print("/");
    Serial.println(DEFAULT_AP_PASS);
    //Cargamos datos por defecto para AP
    strcpy(config.ap_ssid, DEFAULT_AP_SSID);
    Serial.println("copiado1");
    strcpy(config.ap_pass, DEFAULT_AP_PASS);
    Serial.println("copiado2");
  }
  return (strlen(config.net_ssid) > 1);
}

void guardarDatosEEPROM() {

  EEPROM.begin(sizeof(Configuracion));
  delay(10);
  Serial.println("Grabando configuracion");
  EEPROM.put(0, config);
  EEPROM.end();
  delay(10);
}

void resetConfig() {

  Serial.println("RESET configuracion");
  config.ap_activate = false;
  strcpy(config.id, DEFAULT_ID);
  strcpy(config.ap_pass, DEFAULT_AP_PASS);
  strcpy(config.ap_ssid, DEFAULT_AP_SSID);
  config.ap_timeout = 50000;
  strcpy(config.mqtt_user, DEFAULT_MQTT_USER);
  strcpy(config.mqtt_pass, DEFAULT_MQTT_PASS);
  strcpy(config.mqtt_server, DEFAULT_MQTT_SERVER);
  config.mqtt_port = DEFAULT_MQTT_PORT;
  strcpy(config.mqtt_device_id, DEFAULT_MQTT_DEVICE_ID);
  config.mqtt_temper_intervalo_muestreo = DEFAULT_MQTT_PUBLISH_INTERVAL;
  
  sprintf(config.mqtt_humedad_ruta_publicacion,"/%s%s", config.id,DEFAULT_MQTT_HUMIDITY_PATH);
  sprintf(config.mqtt_temper_ruta_publicacion,"/%s%s", config.id,DEFAULT_MQTT_TEMPERATURE_PATH);
  sprintf(config.mqtt_umbral_ruta_publicacion,"/%s%s", config.id,DEFAULT_MQTT_TEMPERATURE_THRESHOLD_PATH);

  strcpy(config.net_pass, "");
  strcpy(config.net_ssid, "");
  
  config.net_manualConfig = false;
  config.net_dns1 = (uint32_t)IPAddress(0, 0, 0, 0);
  config.net_dns2 = (uint32_t)IPAddress(0, 0, 0, 0);
  config.net_IP = (uint32_t)IPAddress(192, 168, 1, 10);
  config.net_gateWay = (uint32_t)IPAddress(198, 168, 1, 1);
  config.net_mask = (uint32_t)IPAddress(255, 255, 255, 0);
  config.temp_objetivo = DEFAULT_TEMP_OBJETIVO;
}



void doReset() {
  display.clearDisplay();
  ESP.reset();
}

void doResetAP() {
  // Acabado este tiempo, forzamos un reset
  display.clearDisplay();
  display.setCursor(0, 0);
  display.print("Tiempo AP finalizado...RESET!");
  display.display();
  delay(2000);
  ESP.reset();
  delay(1000);
}


void modoAhorro() {
  goToSleep(true);
}

void goToSleep(boolean displayMessage) {
  Serial.print("Durmiendo durante ");
  Serial.print(config.mqtt_temper_intervalo_muestreo / 1000);
  Serial.println(" segundos ");
  if (displayMessage) {
    display.setFont();
    display.clearDisplay();
    display.setCursor(0, 0);
    display.print("Ahorro de energia activado");
    display.display();
    delay(2000);
    // Todo: En realidad, hay que apagarla
    display.clearDisplay();
    display.display();
  }
  ESP.deepSleep(config.mqtt_temper_intervalo_muestreo * 1000);
  delay(100); // tarda un poco en entrar en este modo y es conveniente esperar
}


//-----------------------------------------------------------------------------------------------------
// Funciones de gestión los servicios MQTT
//-----------------------------------------------------------------------------------------------------
void conectarMQTT() {

  client.setServer(config.mqtt_server, config.mqtt_port);
  client.setCallback(mqttCallback);
 
  mqtt_connected = client.connected();
  Serial.print("MQTT Conectando a: "); 
  mostrarStatusInit("Conectando a MQTT Server");
  Serial.print(config.mqtt_server);
  Serial.println(config.mqtt_port);
  Serial.print("MQTT Conectado: ");
  Serial.println(mqtt_connected);
  
  for (int i = 0; !mqtt_connected && i < 5; i++) {
    Serial.println(i);  
     mqtt_connected = client.connect(config.id);
     Serial.print(" -- MQTT Conectado: ");
     Serial.println(mqtt_connected);
/*
      
    if (!mqtt_connected) {
      if (strcmp(config.mqtt_user, "") == 0) {
        mqtt_connected = client.connect(config.mqtt_device_id);
          Serial.print(" -- MQTT Conectado: ");
           Serial.println(mqtt_connected);
      }
      else {
        mqtt_connected = client.connect(config.mqtt_device_id, config.mqtt_user, config.mqtt_pass);
               Serial.print(" --2 MQTT Conectado: ");
           Serial.println(mqtt_connected);
      }
    }
  */
  
  }
  
  if (mqtt_connected) {
    // Se subscribe al canal del umbral de temperatura, por si otro 
    // dispositivo cambia este parametro, o para tomar el ultimo valor configurado
    client.subscribe(config.mqtt_umbral_ruta_publicacion);
    Serial.print("Suscrito a MQTT umbral ");
    Serial.println(config.mqtt_umbral_ruta_publicacion);
  }

}


void mqttCallback(char* topic, byte* payload, unsigned int length) {

  Serial.println("Mensaje recibido:  tema: " + String(topic));
  Serial.println("Longitud: " + String(length, DEC));

  String mensaje = "";
  // create character buffer with ending null terminator (string)
  for (unsigned int i = 0; i<length; i++) {
    mensaje += (char)payload[i];
  }
  Serial.println("Mensaje: " + mensaje);
  config.temp_objetivo = atof(mensaje.c_str());
  cambio = true;
}


//-----------------------------------------------------------------------------------------------------
// Funciones de del WEB SERVER
//-----------------------------------------------------------------------------------------------------

void setupServer() {
  /* Set page handler functions */
  server.on("/", wlanPageHandler);
  server.on("/reset", wlanPageReset);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("Servidor HTTP Iniciado");
  webServerOn = true;
}

/* Pagina no encontrada*/
void handleNotFound()
{
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";

  for (uint8_t i = 0; i < server.args(); i++)
  {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }

  server.send(404, "text/plain", message);
}


/* Pagina para resetear la configuración*/
void wlanPageReset() {
  String response_message = "";
  response_message += "<html>";
  response_message += "<head><title>ESP8266 Webserver</title></head>";
  response_message += "<body style=\"background-color:PaleGoldenRod\"><h1><center>Restablecer configuraci&oacute;n</center></h1>";
  response_message += "Se restablecer&aacute;n los ajustes a sus valores por defecto. Reiniciando...";

  response_message += "</body></html>";

  server.send(200, "text/html", response_message);

  // Reiniciamos la configuración a los valores por defecto
  resetConfig();
  // Guardamos los parámetros y reiniciamos
  guardarDatosEEPROM();
  EEPROM.end();
  Serial.flush();
  //Programamos un reset en 5 segundos, una vez servida la página
  timer.setTimeout(5000, doReset);
  return;

}

/* Pagina principal, para configurar parametros del cliente wifi*/
void wlanPageHandler()
{
  // Comprobamos parámetros GET
  if (server.hasArg("ssid"))
  {
    strcpy(config.net_ssid, server.arg("ssid").c_str());
    
    if (server.hasArg("password"))
      strcpy(config.net_pass, server.arg("password").c_str());
    else
      strcpy(config.net_pass, "");

    if (server.hasArg("mqtt_server"))
      strcpy(config.mqtt_server, server.arg("mqtt_server").c_str());
    else
      strcpy(config.mqtt_server, "");      

    if (server.hasArg("id")){
      strcpy(config.id, server.arg("id").c_str());
      sprintf(config.mqtt_humedad_ruta_publicacion,"/%s%s", config.id,DEFAULT_MQTT_HUMIDITY_PATH);
      sprintf(config.mqtt_temper_ruta_publicacion,"/%s%s", config.id,DEFAULT_MQTT_TEMPERATURE_PATH);
      sprintf(config.mqtt_umbral_ruta_publicacion,"/%s%s", config.id,DEFAULT_MQTT_TEMPERATURE_THRESHOLD_PATH);
      config.mqtt_port = DEFAULT_MQTT_PORT;
    }
    else
      strcpy(config.id, "");


    // Guardamos los parámetros y reiniciamos
    guardarDatosEEPROM();
    EEPROM.end();
    Serial.flush();
    //Programamos un reset en 5 segundos, una vez servida la página
    timer.setTimeout(5000, doReset);
    return;
  }

  String response_message = "";
  response_message += "<html>";
  response_message += "<head><title>ESP8266 Webserver</title></head>";
  response_message += "<body style=\"background-color:PaleGoldenRod\"><h1><center>Configuraci&oacute;n WLAN</center></h1>";

  if (WiFi.status() == WL_CONNECTED)
  {
    response_message += "Estado: Conectado<br>";
  }
  else
  {
    response_message += "Estado: Desconectado<br>";
  }

  response_message += "<p>Seleccione una red para conectarse...</p>";

  // Get number of visible access points
  int ap_count = WiFi.scanNetworks();

  if (ap_count == 0)
  {
    response_message += "No se han encontrado puntos de acceso.<br>";
  }
  else
  {
    response_message += F("<form method=\"get\">");
    response_message += F("Id Sensor:<br>");
    response_message += "<input type=\"text\" name=\"id\" value=\"sensor_1\"><br>";
    // Show access points
    for (uint8_t ap_idx = 0; ap_idx < ap_count; ap_idx++)
    {
      response_message += "<input type=\"radio\" name=\"ssid\" value=\"" + String(WiFi.SSID(ap_idx)) + "\">";
      response_message += String(WiFi.SSID(ap_idx)) + " (RSSI: " + WiFi.RSSI(ap_idx) + ")";
      (WiFi.encryptionType(ap_idx) == ENC_TYPE_NONE) ? response_message += " " : response_message += "&#x1f512;";
      response_message += "<br><br>";
    }

    response_message += "Contrase&ntilde;a:<br>";
    response_message += "<input type=\"text\" name=\"password\"><br>";
    response_message += "MQTT Server:<br>";
    response_message += "<input type=\"text\" name=\"mqtt_server\"><br>";
    response_message += "<input type=\"submit\" value=\"Connect\">";
    response_message += "</form>";
  }

  response_message += "</body></html>";

  server.send(200, "text/html", response_message);
}


//-----------------------------------------------------------------------------------------------------
// Funciones de gestión de la Pantalla
//-----------------------------------------------------------------------------------------------------


//--------------------------------------------------------
// Pinta la cabecera común a todas las pantallas
//--------------------------------------------------------
void mostrarCabecera(){
  mostrarIndicadorRed(121, 15, true);
  
  display.setTextSize(1);
  display.setFont(&FreeSans9pt7b);
  display.drawLine(0, 18, 127, 18, WHITE);
  display.setTextColor(WHITE);
  display.setCursor(0, 15);
  display.print(menus[menuActual]);
}

//--------------------------------------------------------
// Muestra el indicador de conexión WIFI
//--------------------------------------------------------
void mostrarIndicadorRed(int posx, int posy, boolean indConectado) {
  // Dibujamos la calidad de la señal wifi
  int rssi = WiFi.RSSI();
  display.drawPixel(posx, posy, WHITE);
  display.drawPixel(posx + 2, posy, WHITE);
  display.drawPixel(posx + 4, posy, WHITE);
  display.drawPixel(posx + 6, posy, WHITE);
  if (!indConectado || WiFi.status() == WL_CONNECTED) {

    if (rssi > -80) {
      display.drawLine(posx, posy, posx, posy - 2, WHITE);
    }
    if (rssi > -75) {
      display.drawLine(posx + 2, posy, posx + 2, posy - 5, WHITE);
    }
    if (rssi > -65) {
      display.drawLine(posx + 4, posy, posx + 4, posy - 8, WHITE);
    }
    if (rssi > -60) {
      display.drawLine(posx + 6, posy, posx + 6, posy - 11, WHITE);
    }
  }
  else if (indConectado) {
    display.drawLine(121, 13, 127, 4, WHITE);
    display.drawLine(121, 4, 127, 13, WHITE);
  }

}

//--------------------------------------------------------
// Muestra la pantalla de Menú que proceda
//--------------------------------------------------------
void mostrarMenu() {

  switch (menuActual) {
  case 0: mostrarTemperatura(); break;
  case 1: mostrarTemperaturaConfig(); break;
  case 2: mostrarRed(); break;
  case 3: mostrarConfig(); break;
  case 4: mostrarRestablecer(); break;
  }
  display.display();
}


void mostrarTemperatura() {
char str_temp[8];
  
  display.clearDisplay();
  //mostrarIndicadorRed(121, 15, true);
  //display.drawLine(0, 18, 127, 18, WHITE);
  
  //display.setFont(&FreeSans9pt7b);
  display.setFont();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  
  display.setCursor(0, 0);
  display.print("TEMPERATURA:");
 
  display.setFont(&FreeSans24pt7b);
  display.setCursor(9, 48);
  dtostrf(temp_actual, 4, 1, str_temp);
  display.print(str_temp);

  display.setFont();
  display.setTextSize(1);
  display.setCursor(100, 20);
  display.setTextColor(WHITE);
  display.print("O");
  display.setTextSize(2);
  display.setCursor(110, 22);
  display.print("C");
  display.setTextSize(1);
  display.setCursor(110, 2);
  
  display.print(modo_arranque);
  
  display.display();
}

void mostrarStatusInit(char *mensaje) {
  display.setFont();
  display.setTextSize(1);
  display.setCursor(0, 56);
  display.setTextColor(WHITE);
  display.print(mensaje);
  display.display();
}
//--------------------------------------------------------
// Pantalla de configuración de la temperatura
//--------------------------------------------------------
void mostrarTemperaturaConfig() {
  display.clearDisplay();
  mostrarCabecera();
  
  display.setFont();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(8, 21);
  display.print("ACTUAL");
  display.setCursor(79, 21);
  display.print("OBJETIVO");
  
  display.setFont(&FreeSans9pt7b);
  char str_temp[8];
  /* 4 is mininum width, 2 is precision; float value is copied onto str_temp*/
  
  display.setCursor(0, 44);
  dtostrf(temp_actual, 4, 2, str_temp);
  display.print(str_temp);
  display.print("c");


  if (modoEdicionPantalla){
    display.drawLine(74, 48, 125, 48, WHITE);
  }
  
  display.setCursor(78, 44);
  dtostrf(config.temp_objetivo, 4, 1, str_temp);
  display.print(str_temp);
  display.print("c");
  
  display.setCursor(0, 58);
  display.setTextColor(WHITE);
  display.setFont();
  display.setTextSize(1);
  display.print(NTP.getTimeDateString());
  display.display();

}

//--------------------------------------------------------
// Pantalla de información de red
//--------------------------------------------------------
void mostrarRed() {
  display.clearDisplay();
  mostrarCabecera();
  
  display.setFont();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 20);
  display.println(estados_wifi[WiFi.status()]);
  display.print("SSID:");
  display.println(WiFi.SSID());
  display.print("IP:  ");
  display.println(WiFi.localIP());
  display.print("Mask:");
  display.println(WiFi.subnetMask());
  display.print("GW:  ");
  display.println(WiFi.gatewayIP());
  mostrarIndicadorRed(121, 34, false);
  display.display();
}


//--------------------------------------------------------
// Pantalla de información de configuración
//--------------------------------------------------------
void mostrarConfig() {
  display.clearDisplay();
  mostrarCabecera();
  
  char str_temp[8];
  display.setFont();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 20);
  display.print("Sensor Id: ");
  display.println(config.id);
  display.print("MQTTSv: ");
  display.println(config.mqtt_server);
  display.print("Estado: ");
  display.println((mqtt_connected ? "Conectado" : "No conectado"));
  display.print("I. muestreo: ");
  display.print(config.mqtt_temper_intervalo_muestreo / 1000);
  display.println("s");
  display.print("Bat: ");
  dtostrf(vcc_perc, 4, 2, str_temp);
  display.print(str_temp);
  display.print("% ");
  dtostrf(vcc, 4, 2, str_temp);
  display.print(str_temp);
  display.print("v");
  display.display();
}


//--------------------------------------------------------
// Pantalla de información de RESET
//--------------------------------------------------------
void mostrarRestablecer() {
  display.clearDisplay();
  mostrarCabecera();
  
  display.setFont();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 27);
  display.println("Pulse para borrar los\ndatos de configura-\ncion y volver a los\nvalores por defecto.");
  display.display();
}

//--------------------------------------------------------
// Lee la información de voltaje de la batería
//--------------------------------------------------------
void getBattery(){

   vcc = ESP.getVcc();
   vcc=vcc/1000.00;
   vcc_perc =  (vcc-VCC_MIN)/(VCC_MAX-VCC_MIN)*100; 

}
/*------------------------------------------------------------------------------------------------------*/
/*
 *   leerDatos() del sensor de temperarura
 */
/*------------------------------------------------------------------------------------------------------*/
void leerDatos() {

  sensors.requestTemperatures();

  float t1 = sensors.getTempCByIndex(0);
  //t1-=3;
  if (t1 < 0) t1 = 25.45;
  
  Serial.print("temperatura:");
  Serial.print(t1);
  Serial.println("º C");

  if (!isnan(t1)) {
    temp_actual = t1;
    cambio = true;
  }
}

/*------------------------------------------------------------------------------------------------------*/
/*
 *   enviarDatos() vía MQTT
 */
/*------------------------------------------------------------------------------------------------------*/
void enviarDatos() {
char mqtt_bateria_ruta_publicacion[64];

  Serial.println("enviarDatos()");
  if (WiFi.status() == WL_CONNECTED) {
    numero_reintentos = 0;
    if (!client.connected()) {
      conectarMQTT();
    }
    if (client.connected()) {
      Serial.println("Publicando por MQTT:");
      client.publish(config.mqtt_temper_ruta_publicacion, String(temp_actual).c_str(), true);
      sprintf(mqtt_bateria_ruta_publicacion,"/%s%s", config.id, DEFAULT_MQTT_VOLTAJE_PATH);
      client.publish(mqtt_bateria_ruta_publicacion, String(vcc).c_str(),true);
      
    }
  }
  else if (modo == MODO_INTERACTIVO) {
    // Se resetea el dispositivo si estando en modo interactivo, falla la conexion repetidamente. 
    // Al resetear, si el problema persiste, arrancará en modo AP
    if (numero_reintentos >= 3) {
      display.clearDisplay();
      display.setFont();
      display.setCursor(0, 0);
      display.setTextColor(WHITE);
      display.print("Se ha perdido la conexión\ndurante un tiempo\nprolongado. RESET");
      display.display();
      delay(5000);
      doReset();
    }
    else {
      numero_reintentos++;
    }
  }

}

/*------------------------------------------------------------------------------------------------------*/
/*
 *   leerEnviarDatos() vía MQTT
 */
/*------------------------------------------------------------------------------------------------------*/

void leerEnviarDatos() {
   leerDatos();
   enviarDatos();  
}


/*------------------------------------------------------------------------------------------------------*/
/*
 *   loop() 
 */
/*------------------------------------------------------------------------------------------------------*/
void loop() {
  
  // En modo interactivo y AP,hay que atender a los timers y al servidor  
  if (modo != MODO_AHORRO) {
    // Ejecutamos los eventos que tengamos en el timer
    timer.run();
    // Atendemos al servidor web
    server.handleClient();
    yield();

    //----------------------- Si estamos en modo interactivo, atentemos la entrada -----------------------
    if (modo == MODO_INTERACTIVO) {

      //Gestiona el pulsador del potenciómetro
      pulsadorVal = digitalRead(ROTARY_SWITCH_PIN);

      if(pulsadorValAnterior== LOW && pulsadorVal == HIGH && !modoEdicionPantalla) 
      {
          modoEdicionPantalla=true;
          cambio=true;
      }
      else if(pulsadorValAnterior== LOW && pulsadorVal == HIGH && modoEdicionPantalla) 
      {
          // Cuando finaliza el modo edición se graba los datos en la EEPROM
          modoEdicionPantalla=false;
          cambio=true;
          guardarDatosEEPROM();
          
      }
      pulsadorValAnterior = pulsadorVal;
      // Fin de la gestion del pulsador
 
      //-------------------- Gestionamos el encoder.----------------------------- 
      // Se divide por 4 para obtener un incremento de 1 en 1
      int32_t pos = encoder.read() / 4;
      // Restamos la posicion anterior. Ese es el incremento leido 
      int8_t incremento = pos - posicion_anterior;

      //------------------- Sí ha habido cambio en el encoder -------------------
      if (incremento != 0) {
        // Ha habido movimiento en el encoder, reseteamos el timer de actividad, y 
        // activa la bandera de cambio para cambiar la pantalla
        timer.restartTimer(timerActividad);
        posicion_anterior += incremento;
        cambio = true;
        
        if (!modoEdicionPantalla) {
        // Si estamos en modo Navegación por pantallas
          menuActual = (menuActual + incremento);
          menuActual = menuActual % numeroMenus;
        }
        else {
        // Si estamos en modo edición de pantalla 
            switch (menuActual) {
              //Pantalla de temperatura
              case 0: break;
              case 1: config.temp_objetivo = config.temp_objetivo + incremento; 
                      break;
              case 2: break;
              case 3: break;
              //Pantalla de Reset
              case 4: 
                      resetConfig();
                      guardarDatosEEPROM();
                      doReset();
                      break;
             } //Fin switch
        }//Fin else
      }//Fin if modoEdicionPantalla
      
      if (cambio) {
        cambio = false;
        mostrarMenu();
      }
     
    } // fin incremento!=0
    
  }
  else {
    // En modo normal,sólo se leen los datos, se envían y se pone a dormir
    leerEnviarDatos();
    goToSleep(false);
  }
  
  client.loop();
  
}



