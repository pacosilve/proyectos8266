/*
 *  This sketch sends data via HTTP GET requests to data.sparkfun.com service.
 *
 *  You need to get streamId and privateKey at data.sparkfun.com and paste them
 *  below. Or just customize this script to talk to other HTTP servers.
 *
 */

#include <ESP8266WiFi.h>
#include <DHT.h>
#include <PubSubClient.h>
#include <SimpleTimer.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>

#define IP 15  // 192.168.1.15
#define IP_ESTATITCA true


//Define el pin para el sensor de temperarura = GPIO 5 -- D1
#define DHT_DATA_PIN 5


#define DHTTYPE DHT22


boolean mqtt_connected = false;
float temp_actual = 0;
float humedad_actual = 0;
uint8_t numero_reintentos = 0;
uint32_t timerActividad;

uint8_t rele1_onoff = 0;
uint8_t rele2_onoff = 0;

#define DEFAULT_AP_SSID  "ESP8266_RELE"
#define DEFAULT_AP_PASS  "12345678"

#define MQTT_SERVER  "192.168.1.100"
#define MQTT_PORT  1883
#define MQTT_DEVICE_ID "ESP8266_RELES2"
#define MQTT_USER "username"
#define MQTT_PASS "password"
#define MQTT_HUM_PATH "/sensor_2/humedad/get"
#define MQTT_TEMP_PATH "/sensor_2/temperatura/get"
#define MQTT_IP_PATH "/reles/ip/get"


#define DEFAULT_PUBLISH_INTERVAL 30000 // 30 segundos


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

// Create an instance of the server
// specify the port to listen on as an argument
// WiFiServer server(80);
// Objeto para manejar el servidor WEB
ESP8266WebServer server(80);


DHT dht(DHT_DATA_PIN, DHTTYPE);

int vcc;

const char* ssid     = "MOVISTAR_2A62";
const char* password = "D29940D1658F43D77279";

//const char* ssid     = "PACOWIFI";
//const char* password = "12345678";

const char * estados_wifi[] = {
  "Inactivo",
  "Sin redes",
  "Busq.compl",
  "Conectado",
  "Error con",
  "Con.perdida",
  "Desconectado" };


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
/*------------------------------------------------------------------------------------------------------*/
/*   setup()                                                                                            */
/*------------------------------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------------------------------*/
void setup() {

  Serial.begin(115200);
  delay(10);

  resetConfig();

  conectarWifi();
  //conectarMQTT();
  // Start the server
  setupServer();
  Serial.println("Server started");
  leerDatosSensorTemperatura();
  
  timer.setInterval(config.intervalo_muestreo, leerDatosSensorTemperatura ); 
  
}

//-----------------------------------------------------------------------------------------------------
void resetConfig() {

  Serial.println("RESET configuracion");
  strcpy(config.net_pass, ssid);
  strcpy(config.net_ssid, password );
  config.intervalo_muestreo= DEFAULT_PUBLISH_INTERVAL;
  strcpy(config.mqtt_server, MQTT_SERVER );
  config.mqtt_port = MQTT_PORT;
  config.net_manualConfig = IP_ESTATITCA;
  config.net_dns1 = (uint32_t)IPAddress(0, 0, 0, 0);
  config.net_dns2 = (uint32_t)IPAddress(0, 0, 0, 0);
  config.net_IP = (uint32_t)IPAddress(192, 168, 1, IP);
  config.net_gateWay = (uint32_t)IPAddress(198, 168, 1, 1);
  config.net_mask = (uint32_t)IPAddress(255, 255, 255, 0);
  
  
}

//-----------------------------------------------------------------------------------------------------
// Funciones del WIFI
//-----------------------------------------------------------------------------------------------------
boolean conectarWifi(void) {
  Serial.print("Conectando a ");
  Serial.print(ssid);
  Serial.print("/");
  Serial.println(password);

  WiFi.begin(ssid, password);
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


//-----------------------------------------------------------------------------------------------------
// Funciones de gestión los servicios MQTT
//-----------------------------------------------------------------------------------------------------
void conectarMQTT() {

  Serial.print("Conectando al Servidor MQTT: ");
  Serial.print(MQTT_SERVER);
  Serial.print(" Port:");
  Serial.println(MQTT_PORT);
  
  client.setServer(MQTT_SERVER, MQTT_PORT);
//  client.setCallback(mqttCallback);

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


/*
  if (mqtt_connected){
    Serial.println("MQTT Conectado. Frecuencia "+ String(config.intervalo_muestreo));
    // Leemos la temperatura cada X seg en modo interactivo.
  }
*/
}


//----------------------------------------------------------------------------------
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

//----------------------------------------------------------------------------------
void publicarDatosMQTT(){
    if (client.connected()) {
      Serial.println("Publicando datos por MQTT.");
      client.publish(MQTT_HUM_PATH, String(humedad_actual).c_str(), true);
      client.publish(MQTT_TEMP_PATH, String(temp_actual).c_str(), true);
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
  else{
    Serial.print("Temp: ");
    Serial.println(temp_actual);
  }  
  enviarDatosHTTP();
//  enviarDatosMQTT();

}


void enviarDatosHTTP(){
  HTTPClient http;
  String url = "http://192.168.1.10/temp?sensor=2&val="+String(temp_actual)+"&humbral=21.30";
  Serial.println("Enviando por HTTP: "+url );
  http.begin(url);
  int httpCode = http.GET();
  Serial.println(httpCode);

}
//-----------------------------------------------------------------------------------------------------
//  reset()
//-----------------------------------------------------------------------------------------------------
void doReset() {
  ESP.reset();
}



//-----------------------------------------------------------------------------------------------------
//  loop()
//-----------------------------------------------------------------------------------------------------
void loop() {
  
  timer.run();   // Timer
  client.loop(); // Loop del MQTT
  server.handleClient(); // Webserver
  yield();
  delay(1000);

}



//-----------------------------------------------------------------------------------------------------
// Funciones de del WEB SERVER
//-----------------------------------------------------------------------------------------------------

void setupServer() {
  /* Set page handler functions */
  server.on("/", web_paginaInicio);
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


/* ---------------------- Pagina Inicio  -------------------------*/
void web_paginaInicio() {
String classCSSR1 = "rojo";
String classCSSR2 = "rojo";


  
  // Prepare the response
   String h = "<!DOCTYPE HTML>\n";
   h += "<html>\n";
   h += "<head>\n";
   h += "<meta http-equiv='refresh' content='15'/>\n";
   h += "<title>ESP8266 </title>\n";
   h += "<style>";
   h += "body { background-color: #000000; font-family: Arial, Helvetica, Sans-Serif; Color: #00FF18; }";
   h += "a:link, a:visited {background-color: #ffffff; color: #000000; border: 1px solid #000000; ";
   h += "display: inline-block; outline: 0; padding: 6px 16px; vertical-align: middle; overflow: hidden; text-decoration: none!important; ";
   h += "text-align: center; cursor: pointer; white-space: nowrap; transition: background-color .3s,color .15s,box-shadow .3s,opacity 0.3s,filter 0.3s;} ";
   h += ".verde {background-color: #00ff00; color: #ffffff; border: 1px solid #000000; ";
   h += "display: inline-block; outline: 0; padding: 6px 36px; vertical-align: middle; overflow: hidden; text-decoration: none!important; ";
   h += "text-align: center; cursor: pointer; white-space: nowrap; } ";
   h += ".rojo {background-color: #ff0000; color: #ffffff; border: 1px solid #000000; ";
   h += "display: inline-block; outline: 0; padding: 6px 36px; vertical-align: middle; overflow: hidden; text-decoration: none!important; ";
   h += "text-align: center; cursor: pointer; white-space: nowrap; } ";
   h += "</style>\n";
   h += "</head>\n";
   h += "<body>\n";


  String s = "<html>\r\n";
  s += "<h1>TEMPERATURA SENSOR 2 <br/><hr>";
  s += "Temperatura: ";
  s +=  temp_actual;
  s += "<br/>Humedad: ";
  s +=  humedad_actual;
  s += " </h1></html>\n";

  server.send(200, "text/html", h + s);
}





