/*

    Programa que gestiona los RELES de la Centralita de los circuitos de la caldera.
    Enciende y apaga los relés en función de las temperaturas de los sensores que se
    reciben vía http. También publica por MQTT los valores de los relés
    Por otro lado también se monitoriza la temperatura del agua del circuito.
 
*/

#include <ESP8266WiFi.h>
#include <DHT.h>
#include <PubSubClient.h>
#include <SimpleTimer.h>
#include <ESP8266WebServer.h>
#include <TimeLib.h>
#include <NtpClientLib.h>
#include <FS.h>

#define DEBUG_NTPCLIENT 1

#define IP 10  // 192.168.1.10
#define IP_ESTATITCA false

#define NTP_SERVER  "pool.ntp.org" ///"213.251.52.234"    ///"pool.ntp.org"

#define ECO 0
#define CONFORT 1

#define AUTO 0
#define MANUAL 1

#define PIN_RELE1 5  //GPIO 5 para el pin D1
#define PIN_RELE2 4  //GPIO 4 para el pin D2

//Define el pin para el sensor de temperarura = GPIO 15
#define DHT_DATA_PIN 15
#define DHTTYPE DHT11

boolean mqtt_connected = false;

// Temperatura/humedad del control del relés
float temp_actual = 0;
float humedad_actual = 0;

// Temperaturas de los sensores
float temp_sensor[2] = {0.0 , 0.0};
float temp_sensor_humbral[2] = {0.0 , 0.0};
int   modoTemperatura[2] = { ECO, ECO };

char DaysOfWeek[][5] = {"Dom", "Lun", "Mar", "Mie", "Jue", "Vie", "Sab" };

uint8_t numero_reintentos = 0;
uint32_t timerActividad;

// Variables de los RELÉS
uint8_t rele_OnOff[2] = {0, 0};
uint8_t pin_reles[2] = {PIN_RELE1, PIN_RELE2};

// WIFI y TCP/IP
#define DEFAULT_AP_SSID  "ESP8266_RELE"
#define DEFAULT_AP_PASS  "12345678"

#define WIFI_SSID        "MOVISTAR_2A62"
#define WIFI_PASS        "D29940D1658F43D77279"

// Constantes  MQTT
#define MQTT_SERVER  "192.168.1.100"
#define MQTT_PORT  1883
#define MQTT_DEVICE_ID "ESP8266_RELES2"
#define MQTT_USER "username"
#define MQTT_PASS "password"
#define MQTT_HUM_PATH "/reles/humedad/get"
#define MQTT_TEMP_PATH "/reles/temperatura/get"
#define MQTT_IP_PATH "/reles/ip/get"

#define MQTT_RELE1_PATH "/reles/rele1/get"
#define MQTT_RELE2_PATH "/reles/rele2/get"
#define MQTT_RELE1_SET_PATH "/reles/rele1/set"
#define MQTT_RELE2_SET_PATH "/reles/rele2/set"

#define MQTT_TEMP_SENSOR1_PATH "/sensor_1/temperatura/get"
#define MQTT_TEMP_SENSOR2_PATH "/sensor_2/temperatura/get"

#define DEFAULT_PUBLISH_INTERVAL 60000


// Estructura con la configuración
typedef struct {
  char id[32];
  char net_ssid[32];
  char net_pass[48];
  char mqtt_server[128];
  uint16_t mqtt_port;
  ulong intervalo_muestreo;
  boolean net_manualConfig;
  uint32_t net_IP;
  uint32_t net_gateWay;
  uint32_t net_mask;
  uint32_t net_dns1;
  uint32_t net_dns2;
} Configuracion;
Configuracion config;

// Estructura programación de la centralitra
typedef struct {
  int id;
  int sala;
  int dia;
  char hora_ini[6];
  char hora_fin[6];
} Reg_data;

Reg_data programacion[300];
int numRegProgramacion = 0;

int modoTrabajo = AUTO; // Valores AUTO y MANUAL

// Objeto para manejar el servidor WEB
ESP8266WebServer server(80);

DHT dht(DHT_DATA_PIN, DHTTYPE);

int vcc;

const char * estados_wifi[] = {
  "Inactivo",
  "Sin redes",
  "Busq.compl",
  "Conectado",
  "Error con",
  "Con.perdida",
  "Desconectado"
};


const char* temp = "10";
const char* hum = "20";

int contador = 0;

// Cliente de red
WiFiClient espClient;
// Cliente MQTT
PubSubClient client(espClient);
// Objeto temporizador, para lanzar funciones de forma programada
SimpleTimer timer;


/*------------------------------------------------------------------------------------------------------*/
/*                                             setup()                                                  */
/*------------------------------------------------------------------------------------------------------*/
void setup() {
  static WiFiEventHandler e1;
  // Define la funcion del evento de la wifi para conectar por NTP
  e1 = WiFi.onStationModeGotIP(onSTAGotIP);

  bool result = SPIFFS.begin();
  Serial.begin(115200);
  delay(10);

  pinMode(pin_reles[0], OUTPUT);
  digitalWrite(pin_reles[0], LOW);

  pinMode(pin_reles[1], OUTPUT);
  digitalWrite(pin_reles[1], LOW);

  initConfig();
  conectarWifi();

  // Start the server
  setupServer();
  Serial.println("Server started");

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

  //  conectarMQTT();

  initProgramacion();


}

//-----------------------------------------------------------------------------------------------------
void initConfig() {

  Serial.println("RESET configuracion");
  strcpy(config.net_pass, WIFI_SSID);
  strcpy(config.net_ssid, WIFI_PASS);
  config.intervalo_muestreo = DEFAULT_PUBLISH_INTERVAL;
  strcpy(config.mqtt_server, MQTT_SERVER );
  config.mqtt_port = MQTT_PORT;
  config.net_manualConfig = IP_ESTATITCA;
  config.net_dns1 = (uint32_t)IPAddress(8, 8, 8, 8);  //DNSs de Google
  config.net_dns2 = (uint32_t)IPAddress(8, 8, 4, 4);
  config.net_IP = (uint32_t)IPAddress(192, 168, 1, IP);
  config.net_gateWay = (uint32_t)IPAddress(198, 168, 1, 1);
  config.net_mask = (uint32_t)IPAddress(255, 255, 255, 0);

}

//-----------------------------------------------------------------------------------------------------
// Funciones del WIFI
//-----------------------------------------------------------------------------------------------------
boolean conectarWifi(void) {
  Serial.print("Conectando a ");
  Serial.print(WIFI_SSID);
  Serial.print("/");
  Serial.println(WIFI_PASS);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  if (config.net_manualConfig == true) {
    Serial.println("Configuracion manual");
    WiFi.config(
      IPAddress(config.net_IP),
      IPAddress(config.net_gateWay),
      IPAddress(config.net_mask),
      IPAddress(config.net_dns1),
      IPAddress(config.net_dns2));
  }
  // WiFi.setAutoConnect(true);

  int c = 0;
  Serial.println("Esperando conexion Wifi");
  while (c < 80) {
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
  NTP.begin(NTP_SERVER, 1, true);
  NTP.setInterval(63);
}


//-----------------------------------------------------------------------------------------------------
// Funciones de gestión los servicios MQTT
//-----------------------------------------------------------------------------------------------------
void conectarMQTT() {

  Serial.print("Conectando al Servidor MQTT: ");
  Serial.print(MQTT_SERVER);
  Serial.print(" Port:");
  Serial.println(MQTT_PORT);

  client.setServer(MQTT_SERVER, MQTT_PORT);
  client.setCallback(mqttCallback);

  mqtt_connected = client.connected();
  for (int i = 0; !mqtt_connected && i < 5; i++) {
    if (!mqtt_connected) {
      if (strcmp(MQTT_USER, "") == 0) {
        mqtt_connected = client.connect(MQTT_DEVICE_ID);
      }
      else {
        mqtt_connected = client.connect(MQTT_DEVICE_ID, MQTT_USER, MQTT_PASS);
      }
    }
  }


  if (mqtt_connected) {
    Serial.println("MQTT Conectado. Frecuencia " + String(config.intervalo_muestreo));
    client.subscribe(MQTT_RELE1_SET_PATH);
    client.subscribe(MQTT_RELE2_SET_PATH);

    // Leemos la temperatura cada X seg en modo interactivo.
    timer.setInterval(config.intervalo_muestreo, leerDatosSensorTemperatura );
  }

}


/* ---------------------- mqttCallback() ---------------------*/
void mqttCallback(char* topic, byte* payload, unsigned int length) {

  //  Serial.println("Longitud: " + String(length, DEC));

  String mensaje = "";
  // create character buffer with ending null terminator (string)
  for (unsigned int i = 0; i < length; i++) {
    mensaje += (char)payload[i];
  }

  Serial.println("Mensaje recibido:  TOPIC: " + String(topic) + " Menssaje: " + mensaje);

  if ( !strcmp(topic, MQTT_RELE1_SET_PATH) ) {
    if (mensaje.equals("0"))  digitalWrite(pin_reles[0], LOW);
    if (mensaje.equals("1"))  digitalWrite(pin_reles[0], HIGH);
    publicarDatosMQTT();
  }

  if ( !strcmp(topic, MQTT_RELE2_SET_PATH) ) {
    if (mensaje.equals("0"))  digitalWrite(pin_reles[1], LOW);
    if (mensaje.equals("1"))  digitalWrite(pin_reles[1], HIGH);
    publicarDatosMQTT();
  }


  if ( !strcmp(topic, MQTT_TEMP_SENSOR1_PATH) ) {
    temp_sensor[0] = atof(mensaje.c_str()) ;
  }

  if ( !strcmp(topic, MQTT_TEMP_SENSOR2_PATH) ) {
    temp_sensor[1] = atof(mensaje.c_str()) ;
  }


}

/* ---------------------- enviarDatosMQTT() ---------------------*/
void enviarDatosMQTT() {
  if (WiFi.status() == WL_CONNECTED) {
    numero_reintentos = 0;
    if (!client.connected()) {
      conectarMQTT();
    }
    publicarDatosMQTT();
  }
  else {
    // Se resetea el dispositivo si estando en modo interactivo, falla la conexion repetidamente.
    // Al resetear, si el problema persiste, arrancará en modo AP
    if (numero_reintentos >= 10) {
      Serial.println("Reset por MQTT error.");
      doReset();
    }
    else {
      numero_reintentos++;
    }
  }

}

/* ---------------------- publicarDatosMQTT() ---------------------*/
void publicarDatosMQTT() {
  if (client.connected()) {
    Serial.println("Publicando datos por MQTT.");
    client.publish(MQTT_HUM_PATH, String(humedad_actual).c_str(), true);
    client.publish(MQTT_TEMP_PATH, String(temp_actual).c_str(), true);
    client.publish(MQTT_RELE1_PATH, String(digitalRead(pin_reles[0])).c_str(), true);
    client.publish(MQTT_RELE2_PATH, String(digitalRead(pin_reles[1])).c_str(), true);
    Serial.println("MQTT publish TEMP. ");
  }
}

//-----------------------------------------------------------------------------------------------------
// Gestión del Sensor de Temperatura
//-----------------------------------------------------------------------------------------------------
void leerDatosSensorTemperatura() {

  // Lectura del sensor de temperatura

  humedad_actual = dht.readHumidity();
  temp_actual = dht.readTemperature();
  if (isnan(humedad_actual) || isnan(temp_actual)) {
    Serial.println("Temp: #");
  }
  else {
    Serial.print("Temp: ");
    Serial.println(temp_actual);
  }
  enviarDatosMQTT();
}



/* ---------------------- doReset() ---------------------*/
void doReset() {
  ESP.reset();
}

//-----------------------------------------------------------------------------------------------------
//                                         loop()
//-----------------------------------------------------------------------------------------------------
void loop() {

  timer.run();             // Timer
  client.loop();           // Loop del MQTT
  server.handleClient();   // Webserver

  procesaProgramacion();

  yield();
  delay(2000);

}

//-----------------------------------------------------------------------------------------------------
// Funciones de del WEB SERVER
//-----------------------------------------------------------------------------------------------------
void setupServer() {
  /* Set page handler functions */
  server.on("/", web_paginaInicio);
  server.on("/programacion", web_pagProgramacion);
  server.on("/set", web_set_rele);
  server.on("/temp", web_set_temp);
  server.on("/prog", web_set_prog);
  server.on("/modo", web_set_modo);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Servidor HTTP Iniciado");

}

/* ---------------------- Pagina no encontrada ---------------------*/
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

/* ---------------------- printCssStyle() ---------------------*/
String printCssStyle() {
  String h;
  
  h += F("<style>"); //33ff55
  h += F("body { background-color: #000000; font-size: 2.1em; font-family: Arial, Helvetica, Sans-Serif; Color: #84aeff; margin: 10px;}\n");
  h += F("a.btn {font-size:1.2em; background-color: #ffffff; color: #000000; border: 0px solid #000000; vertical-align: middle;");
  h += F("outline: 0; padding: 6px 16px; text-decoration: none!important; -webkit-border-radius: 8px; -moz-border-radius: 8px; border-radius: 8px;");
  h += F("text-align: center; cursor: pointer; white-space: nowrap; transition: background-color .3s,color .15s,box-shadow .3s,opacity 0.3s,filter 0.3s;}\n");
  h += F(".verde {font-size:1.2em; background-color: #00ff00; color: #ffffff;");
  h += F("display: inline-block;  padding: 6px 76px; vertical-align: middle; overflow: hidden; text-decoration: none!important; ");
  h += F("text-align: center; cursor: pointer; white-space: nowrap; } \n");
  h += F(".rojo {font-size:1.2em; background-color: #ff0000; color: #ffffff;  ");
  h += F("display: inline-block;  padding: 6px 76px; vertical-align: middle; overflow: hidden; text-decoration: none!important; ");
  h += F("text-align: center; cursor: pointer; white-space: nowrap; } ");
  h += F("a.btnBorrar {font-size: 0.8em; background-color: #ffffff; color: #000000; -webkit-border-radius: 5px; -moz-border-radius: 5px; border-radius: 5px;");
  h += F("padding: 2px 16px; vertical-align: middle; overflow: hidden; text-decoration: none!important; ");
  h += F("text-align: center; cursor: pointer; white-space: nowrap; } \n");
  h += F("table {border: 0px solid white; width: 100%;}");
  h += F("b, tr.th {Color: #ffffff;} \n");
  h += F("td, th {text-align: left; padding: 18px;  border-bottom: 1px solid #ddd; } \n");
  h += F("form, input, button {font-size:0.8em; }\n");
  h += F("tr:nth-child(even).prog {background-color: #f2f2f2}");
  h += F(".formulario {font-size:0.8em; color: #ffffaa;}\n");
  h += F(".menu { color: #000000; margin: 3px; padding: 8px; overflow: hidden; border: 0px solid #ccc; vertical-align: middle; background-color: #60bdd2; text-decoration: none!important;}\n");
  h += F("@media only screen and (min-device-width: 375px) and (max-device-width: 667px) and (orientation : portrait) {");
  h += F("body {font-size: 3.1em; }\n");
  h += ("}");
  h += "</style>\n";
  return (h);
}

/* ------------------------ printMenu() --------------------------*/
String printMenu() {
  String s;
  s += F("<br><div style=\"width:100%;\" class=\"menu\">&gt;&gt; ");
  s += F("<a class=\"menu\" href=\"./\">Inicio</a> | ");
  s += F("<a class=\"menu\" href=\"./programacion\">Programaci&oacute;n</a> ");
  s += F("</div>");
  return (s);
}

/* ---------------------- printEncabezado() ---------------------*/
String printEncabezado() {
  String s;
  char fechahora[30];
  time_t tiempo;

  s += F("<b>CONTROL CALEFACCI&Oacute;N</b>");
  s += F("<div style=\"float:right;  font-size:0.9em; color:#ffffff \">");
  tiempo = NTP.getTime();
  sprintf(fechahora, "%s %02d:%02d", DaysOfWeek[dayOfWeek(NTP.getTime()) - 1], hour(tiempo),  minute(tiempo));
  s += fechahora;
  s += F("</div>");
  s += F("<hr/>");
  return (s);
}

/* ---------------------- Pagina Incio  -------------------------*/
void web_paginaInicio() {
  time_t tiempo;
  String classCSSR[2] = {"rojo", "rojo"};

  if (digitalRead(pin_reles[0])) classCSSR[0] = "verde";
  if (digitalRead(pin_reles[1])) classCSSR[1] = "verde";

  String h = "<!DOCTYPE HTML>\n";
  h += "<html>\n";
  h += "<head>\n";
  h += "<meta http-equiv='refresh' content='60'/>\n";
  h += "<title>ESP8266 - CENTRALITA </title>\n";
  h += printCssStyle();
  h += "</head>\n";
  h += "<body>\n";

  String classModo =         (modoTrabajo == AUTO ? "verde" : "rojo");
  String modoTrabajoTexto =  (modoTrabajo == AUTO ? "AUTO" : "MANUAL");
  String s = "<html>\r\n";

  s += printEncabezado();
  s += "Programa: &nbsp; ";
  s += "<div class=\"" + classModo + "\">" + modoTrabajoTexto + "</div> ";
  s += "<a class=\"btn\" href=\"./modo\">Cambiar</a>";
  s += "<hr>";
  s += "Temp. agua: <b>";
  s +=  temp_actual;
  s += "&deg;</b>";
  s += "<hr/>";
  // Table
  s += "<table>";
  for (int i = 0; i < 2; i++) {
    //Fila Temperatura
    s += "<tr><td rowspan=3 style=\"vertical-align:top;\"><b>Sal&oacute;n " + String(i + 1) + ":</b> </td><td>Temp: <b>";
    s += temp_sensor[i];
    s += "&deg; </b> Humbral: <b>";
    s += temp_sensor_humbral[i];
    s += "&deg;</b></td>";
    s += "</tr>";
    //Fila modo ECO/CONFORT
    s += "<tr>";
    s += "<td>Modo: <b>";
    s += (modoTemperatura[i] == ECO ? "<b style=\"color:#00e3ff;\">ECO</b>" : "<b style=\"color:#ff6200;\">CONFORT</b>");
    s += "</b></td> ";
    s += "</tr>";
    //Fila botones ON/OFF
    s += "<tr> ";
    s += "<td><div class=\"" + classCSSR[i] + "\">";
    s +=  String(digitalRead(pin_reles[i])).c_str();
    s += "</div> &nbsp;&nbsp;";
    s += "<a class=\"btn\" href=\"./set?rele=" + String(i + 1) + "&val=1\">On</a> ";
    s += "<a class=\"btn\" href=\"./set?rele=" + String(i + 1) + "&val=0\">Off</a> ";
    s += "</td></tr>";
  }


  /*
    //Fila 2
    s += "<tr><td rowspan=3 style=\"vertical-align:top;\"><b>Sal&oacute;n "+String(2)+":</b> </td><td>Temp: <b>";
    s += temp_sensor[1];
    s += "&deg; </b> Humbral: <b>";
    s += temp_sensor_humbral[1];
    s += "&deg;</b></td>";
    s += "</tr>";
    s += "<tr>";
    s += "<td>Modo :<b>";
    s += (modoTemperatura[1] == ECO ? "ECO" : "CONFORT");
    s += "</b></td>";
    s += "</tr>";
    s += "<tr> ";
    s += "<td ><div class=\""+ classCSSR[1] + "\">";
    s +=  String(digitalRead(pin_reles[1])).c_str();
    s += "</div> &nbsp;&nbsp;";
    s += "<a class=\"btn\" href=\"./set?rele="+String(2)+"&val=1\">On</a> ";
    s += "<a class=\"btn\" href=\"./set?rele="+String(2)+"&val=0\">Off</a> ";
    s += "</td></tr>";
  */

  s += "</table>";
  // --- Menu
  s += printMenu();
  s += "</html>\n";

  server.send(200, "text/html", h + s);
}

/* ---------------------- Página Programación  -------------------------*/
void web_pagProgramacion() {
  String classCSSR1 = "rojo";
  String classCSSR2 = "rojo";


  // Prepare the response
  String h = "<!DOCTYPE HTML>\n";
  h += "<html>\n";
  h += "<head>\n";
  h += "<title>ESP8266 </title>\n";
  h += printCssStyle();
  h += "</head>\n";
  h += "<body>\n";

  String classModo =         (modoTrabajo == AUTO ? "verde" : "rojo");
  String modoTrabajoTexto =  (modoTrabajo == AUTO ? "AUTO" : "MANUAL");
  String s = "<html>\r\n";
  s += printEncabezado();
  s += "Programa: &nbsp; ";
  s += "<div class=\"" + classModo + "\">" + modoTrabajoTexto + "</div> ";
  s += "<a class=\"btn\" href=\"./modo\">Cambiar</a>";
  s += "<hr>";  //--------------

  for (int k = 1; k <= 2; k++) {
    s += "<div>";
    s += "<b>Sal&oacute;n " + String(k) + ": </b> &nbsp;&nbsp; ";
    s += (modoTemperatura[k-1] == ECO ? "<b style=\"color:#00e3ff;\">ECO</b>" : "<b style=\"color:#ff6200;\">CONFORT</b>");
    if (numRegProgramacion > 0) {
      s += "<table>";
      s += "<tr class=\"prog\"><th>D&iacute;a</th><th>Hora Ini</th><th>Hora Fin</tn><th>Op</th></tr>";
      for (int i = 0; i < numRegProgramacion; i++) {
        if (programacion[i].sala == k) {
          s += "<tr><td>";
          s += DaysOfWeek[ (programacion[i].dia % 7) ];
          s += "</td><td>";
          s += programacion[i].hora_ini;
          s += "</td><td>";
          s += programacion[i].hora_fin;
          s += "</td><td style=\"text-align: center;\">";
          s += "<a class=\"btnBorrar\" href=\"./prog?deleteitem=";
          s += i + 1;
          s += "\">Borrar</a> ";
          s += "</td></tr>";
        }
      }
      s += "<table><br>";
    }
    s += "</div>";
  }
  s += "<br><div class=\"formulario\">";
  s += "<form class=\"formulario\" method=\"get\"  action=\"prog\">\n";
  s += "D&iacute;a: <input type=\"text\" name=\"dia\" size=\"3\" value=\"1\"> ";
  s += "Sala: <input type=\"text\" name=\"sala\" size=\"3\" value=\"1\"> ";
  s += "Hora: <input type=\"text\" name=\"horainifin\" size=\"11\" value=\"08:00,23:00\"> ";
  s += "<input type=\"submit\" value=\"A&ntilde;adir\">";
  s += "</form>";
  s += "</div>";

  // --- Menu
  s += printMenu();
  s += "</html>\n";

  server.send(200, "text/html", h + s);
}

/* ---------------------- Enciende/Apaga un relé -------------------------*/
void web_set_rele() {

  if (server.hasArg("rele")) {
    String releID = server.arg("rele").c_str();
    if (server.hasArg("val")) {
      String releVALUE = server.arg("val").c_str();

      if ( releID.equals("1") ) {
        if (releVALUE.equals("0"))  digitalWrite(pin_reles[0], LOW);
        if (releVALUE.equals("1"))  digitalWrite(pin_reles[0], HIGH);
      }
      if ( releID.equals("2") ) {
        if (releVALUE.equals("0"))  digitalWrite(pin_reles[1], LOW);
        if (releVALUE.equals("1"))  digitalWrite(pin_reles[1], HIGH);
      }
      publicarDatosMQTT();
    }
  }

  // Prepare the response
  String h = "<!DOCTYPE HTML>\n";
  h += "<html>\n";
  h += "<head>\n";
  h += "<meta http-equiv=\"Refresh\" content=\"0; url=.\">";
  h += "<style>body { background-color: #000000; font-family: Arial, Helvetica, Sans-Serif; Color: #FFFF00; }</style>\n";
  h += "</head>\n";
  h += "<body>\n";

  String s = "<html>\r\n";
  s += "<br/>Sal&oacute;n 1: ";
  s +=  String(digitalRead(pin_reles[0])).c_str();
  s += "<br/>Sal&oacute;n 2: ";
  s +=  String(digitalRead(pin_reles[1])).c_str();
  s += "</html>\n";

  server.send(200, "text/html", h + s);
}


/* ---------------------- Pagina Set Temperatura  -------------------------*/
void web_set_temp() {
  String sensorVALUE;

  if (server.hasArg("sensor")) {
    String sensorID = server.arg("sensor").c_str();
    if (server.hasArg("val")) {
      sensorVALUE = server.arg("val").c_str();

      if ( sensorID.equals("1") ) {
        temp_sensor[0] = atof(sensorVALUE.c_str());
      }
      if ( sensorID.equals("2") ) {
        temp_sensor[1] = atof(sensorVALUE.c_str());
      }
    }
    if (server.hasArg("humbral")) {
      sensorVALUE = server.arg("humbral").c_str();

      if ( sensorID.equals("1") ) {
        temp_sensor_humbral[0] = atof(sensorVALUE.c_str());
      }
      if ( sensorID.equals("2") ) {
        temp_sensor_humbral[1] = atof(sensorVALUE.c_str());
      }
    }
  }

  String h = "<!DOCTYPE HTML>\n<html>\n<head>\n";
  h += "<meta http-equiv=\"Refresh\" content=\"0; url=.\">";
  h += "</head>\n<body>\n<h1>OK</h1></html>\n";

  server.send(200, "text/html", h);
}

/* ---------------------- Pagina set programación  -------------------------*/
void web_set_prog() {
  char cad[100];
  int id;

  if (server.hasArg("prog")) {
    strcpy(cad , server.arg("prog").c_str());
    Serial.printf("Cadena %s \n", cad);
    set_fila(cad, &programacion[numRegProgramacion]);
    numRegProgramacion++;
  }

  if (server.hasArg("dia")) {
    sprintf(cad, "1,%s,%s,%s", server.arg("sala").c_str(), server.arg("dia").c_str(), server.arg("horainifin").c_str());
    Serial.printf("Cadena %s \n", cad);
    set_fila(cad, &programacion[numRegProgramacion]);
    numRegProgramacion++;
  }

  if (server.hasArg("deleteitem")) {
    id = atoi(server.arg("deleteitem").c_str());
    arrayProgramacionDeleteItem(id);
  }

  if (server.hasArg("deleteall")) {
    arrayDeleteProgramacion();
  }

  arrayProgramacionSort();
  arraySaveProgramacion();

  String h = "<!DOCTYPE HTML>\n<html>\n<head>\n";
  h += "<meta http-equiv=\"Refresh\" content=\"0; url=./programacion\">";
  h += "</head>\n<body>\n<h1>OK</h1></html>\n";

  server.send(200, "text/html", h);

}

/* ---------------------- Pagina set modo -------------------------*/
void web_set_modo() {

  if (modoTrabajo == ECO)
    modoTrabajo = CONFORT;
  else
    modoTrabajo = ECO;

  String h = "<!DOCTYPE HTML>\n<html>\n<head>\n";
  h += "<meta http-equiv=\"Refresh\" content=\"0; url=.\">";
  h += "</head>\n<body>\n<h1>OK</h1></html>\n";

  server.send(200, "text/html", h);

}

//-----------------------------------------------------------------------------------------------------
//                                         procesaProgramacion()
//-----------------------------------------------------------------------------------------------------
void procesaProgramacion() {
  int    dia, hora, minuto, sala;
  float  temp_sala, temp_humbral;
  char   hora_min[8];
  int    salas[2] = {ECO, ECO};
  time_t tiempo;
  String strTemp, strTempHumbral ;

  tiempo = NTP.getTime();
  dia = dayOfWeek(tiempo) - 1;
  if(dia == 0) dia= 7;
  hora = hour(tiempo);
  minuto = minute(tiempo);

  sprintf(hora_min, "%02d:%02d", hora, minuto);
  modoTemperatura[0] = ECO;
  modoTemperatura[1] = ECO;


  // Recorre la programación para ver si toca encender la calefacción
  for (int i = 0; i < numRegProgramacion; i++) {
   //Serial.printf("Sala %d >>>>> Día %d Hora  %s < %s >%s \n", programacion[i].sala, dia,  programacion[i].hora_ini, hora_min, programacion[i].hora_fin );

    if ( (programacion[i].dia == dia) &&
         (strcmp(hora_min, programacion[i].hora_ini) > 0) &&
         (strcmp(hora_min, programacion[i].hora_fin) < 0)) {
      //Serial.printf("Sala %d Mismo Día %d Hora  %s < %s >%s \n", programacion[i].sala, dia,  programacion[i].hora_ini, hora_min, programacion[i].hora_fin );
      sala = programacion[i].sala;
      modoTemperatura[sala - 1] = CONFORT;
    }
  }

  if (modoTrabajo == AUTO){ 
      //Enciende o apaga el rele en función de la temperatura con el humbral
      for (sala = 1; sala <= 2; sala++) {
        temp_sala    = temp_sensor[sala - 1];
        temp_humbral = temp_sensor_humbral[sala - 1] - (modoTemperatura[sala - 1] == ECO ? 5.0 : 0.5);
    
        strTemp = temp_sala;
        strTempHumbral = temp_humbral;
        // Serial.printf(" Sala %d, Temp: %s Humbral %s \n", sala, strTemp.c_str(), strTempHumbral.c_str());
        if ( temp_sala < temp_humbral ) {
          //Enciende el RELE de la Sala
          setRele(sala, HIGH);
        }
        if ( temp_sala >= temp_humbral) {
          //APAGA el RELE de la Sala
          setRele(sala, LOW);
        }
      }
  }


}

//-----------------------------------------------------------------------------------------------------
//  setRele()
//-----------------------------------------------------------------------------------------------------
void setRele(int idRele, int set) {

  if (digitalRead(pin_reles[idRele - 1]) != set) {
    Serial.printf("setRele(): Cambiando el RELE %d al valor %d\n", idRele, set);
    digitalWrite(pin_reles[idRele - 1], set);
    publicarDatosMQTT();
  }
}

//-----------------------------------------------------------------------------------------------------
// Funciones de gestión del ARRAY de Programación
//-----------------------------------------------------------------------------------------------------
void initProgramacion() {
  char *cadena;
  Serial.println("initProgramacion()");
  arrayLoadProgramacion();
  arrayProgramacionSort();

  arraySaveProgramacion();
}

/* ---------------------- Carga el ARRAY del fichero  -------------------------*/
void arrayLoadProgramacion() {
  char cadena[100];

  // open file for reading
  File f = SPIFFS.open("/config.txt", "r");
  if (!f) {
    Serial.println("File open failed for read");
  }
  Serial.println("====== Reading from SPIFFS file =======");
  int i = 0;

  String s = f.readStringUntil('\n');
  while ( !s.equals("") ) {
    Serial.println(s);
    strcpy(cadena, s.c_str());
    set_fila(cadena, &programacion[i]);
    i++;
    s = f.readStringUntil('\n');
  }
  numRegProgramacion = i;

}

/* ---------------------- Salva el ARRAY en fichero  -------------------------*/
void arraySaveProgramacion() {
  char cadena[100];

  File f = SPIFFS.open("/config.txt", "w");
  if (!f) {
    Serial.println("file creation failed for write");
  }
  for (int i = 0; i < numRegProgramacion; i++) {
    sprintf(cadena, "%d,%d,%d,%s,%s", programacion[i].id, programacion[i].sala, programacion[i].dia,
            programacion[i].hora_ini, programacion[i].hora_fin);
    f.println(cadena);

  }
  f.close();
}

/* ---------------------- Borra el ARRAY en fichero  -------------------------*/
void arrayDeleteProgramacion() {
  File f = SPIFFS.open("/config.txt", "w");
  if (!f) {
    Serial.println("file creation failed for write");
  }
  f.close();
  numRegProgramacion = 0;
}

/* ---------------------- Carga una fila de una cadena  -------------------------*/
void set_fila(char *cad, Reg_data *reg) {
  char str[50];
  char *delim = ",";
  char *token;
  int i = 0;

  strcpy(str, cad);
  Serial.printf(" Set_fila %s\n", str);
  for (token = strtok(str, delim); token; token = strtok(NULL, delim))
  {
    switch (i) {
      case 0:
        reg->id = atoi(token);
        break;
      case 1:
        reg->sala = atoi(token) ;
        break;
      case 2:
        reg->dia = atoi(token);
        break;
      case 3:
        strcpy(reg->hora_ini, token);
        break;
      case 4:
        strcpy(reg->hora_fin, token);
        break;
    }
    i++;
  }
}

/* ---------------------- Ordena el Array  -------------------------*/
void arrayProgramacionSort()
{
  int i, j ;
  Reg_data temp;

  for (i = 0; i < (numRegProgramacion - 1); ++i)
  {
    for (j = 0; j < numRegProgramacion - 1 - i; ++j )
    {
      if (programacion[j].sala > programacion[j + 1].sala)
      {
        regCopy(&temp, &programacion[j + 1]);
        regCopy(&programacion[j + 1], &programacion[j]);
        regCopy(&programacion[j], &temp);
      }
      else if ( (programacion[j].sala == programacion[j + 1].sala) && (programacion[j].dia > programacion[j + 1].dia )) {
        regCopy(&temp, &programacion[j + 1]);
        regCopy(&programacion[j + 1], &programacion[j]);
        regCopy(&programacion[j], &temp);
      }
      else if ( (programacion[j].sala == programacion[j + 1].sala) && (programacion[j].dia == programacion[j + 1].dia )
                &&  (strcmp(programacion[j].hora_ini, programacion[j + 1].hora_ini) > 0)  ) {
        regCopy(&temp, &programacion[j + 1]);
        regCopy(&programacion[j + 1], &programacion[j]);
        regCopy(&programacion[j], &temp);
      }
    }
  }

  for (i = 0; i < numRegProgramacion; ++i) {
    programacion[i].id = i + 1;
    regPrint(&programacion[i]);
  }

}

/* ---------------------- Borra un elemento del Array  -------------------------*/
void arrayProgramacionDeleteItem(int position) {
  int i;

  if ( position >= numRegProgramacion + 1 )
    Serial.println("Deletion not possible.");
  else
  {
    Serial.printf("Borrando item %d\n", position);
    for ( i = position - 1 ; i < numRegProgramacion - 1 ; i++ )
      regCopy(&programacion[i], &programacion[i + 1]);

    numRegProgramacion--;
    for (i = 0; i < numRegProgramacion; ++i) {
      programacion[i].id = i + 1;
      regPrint(&programacion[i]);
    }
    Serial.println("Fin borrado");
  }
}

/* ---------------------- Copia un Registro  -------------------------*/
void regCopy(Reg_data *dest, Reg_data *ori) {

  dest->id = ori->id;
  dest->sala = ori->sala;
  dest->dia = ori->dia;
  strcpy(dest->hora_ini, ori->hora_ini);
  strcpy(dest->hora_fin, ori->hora_fin);

}

/* ---------------------- Copia un Registro  -------------------------*/
void regPrint(Reg_data *reg) {

  Serial.printf(">>> %d,%d,%d,%s,%s \n", reg->id, reg->sala, reg->dia,
                reg->hora_ini, reg->hora_fin);
}

/* ---------------------- Copia un Registro  -------------------------*/
void regInit(Reg_data *dest) {

  dest->id = 0;
  dest->sala = 0;
  dest->dia = 0;
  strcpy(dest->hora_ini, "");
  strcpy(dest->hora_fin, "");
}
