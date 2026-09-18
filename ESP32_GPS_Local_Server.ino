#include <WiFi.h>
#include <HTTPClient.h>

/*
  REAL-TIME GPS VEHICLE TRACKING
  STM32F103C6T6A -> ESP32 -> LOCAL WEB SERVER

  ESP32 UART2:
    GPIO16 = RX2 <- STM32 USART2 TX
    GPIO17 = TX2 -> STM32 USART2 RX (optional)
    GND    = common ground

  Change the Wi-Fi and server settings below.
*/

// ---------- WIFI ----------
const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// PC IP address running your local Flask/XAMPP server
const char* SERVER_IP = "192.168.1.10";
const uint16_t SERVER_PORT = 5000;
const char* SERVER_PATH = "/gpsdata";

// ---------- UART ----------
#define STM32_RX 16
#define STM32_TX 17

HardwareSerial STM32Serial(2);

// ---------- GPS ----------
String nmeaLine = "";
double latitude = 0.0;
double longitude = 0.0;
double speedKmph = 0.0;
String utcTime = "";
String utcDate = "";
bool gpsFix = false;
bool newGPSData = false;

unsigned long lastSend = 0;
unsigned long lastWiFiCheck = 0;

const unsigned long SEND_INTERVAL = 2000;
const unsigned long WIFI_CHECK_INTERVAL = 5000;

// ---------- NMEA coordinate conversion ----------
double nmeaToDecimal(String value, char direction)
{
  if (value.length() < 3) return 0.0;

  double raw = value.toDouble();
  int degrees = (int)(raw / 100.0);
  double minutes = raw - (degrees * 100.0);

  double result = degrees + minutes / 60.0;

  if (direction == 'S' || direction == 'W')
    result = -result;

  return result;
}

// ---------- NMEA checksum ----------
bool checkChecksum(String sentence)
{
  int star = sentence.indexOf('*');

  // Accept sentences without checksum.
  if (star < 0) return true;
  if (star + 2 >= sentence.length()) return false;

  byte calculated = 0;

  for (int i = 1; i < star; i++)
    calculated ^= (byte)sentence[i];

  String receivedText = sentence.substring(star + 1, star + 3);
  byte received = (byte)strtol(receivedText.c_str(), NULL, 16);

  return calculated == received;
}

// ---------- Parse GPRMC/GNRMC ----------
bool parseRMC(String sentence)
{
  if (!sentence.startsWith("$GPRMC") &&
      !sentence.startsWith("$GNRMC"))
    return false;

  if (!checkChecksum(sentence))
    return false;

  int star = sentence.indexOf('*');
  if (star >= 0)
    sentence = sentence.substring(0, star);

  if (sentence.startsWith("$"))
    sentence.remove(0, 1);

  String field[12];
  int count = 0;
  int start = 0;

  for (int i = 0; i <= sentence.length(); i++)
  {
    if (i == sentence.length() || sentence[i] == ',')
    {
      if (count < 12)
        field[count++] = sentence.substring(start, i);

      start = i + 1;
    }
  }

  if (count < 10) return false;

  utcTime = field[1];

  char status = field[2].length() ? field[2][0] : 'V';

  if (status != 'A')
  {
    gpsFix = false;
    return true;
  }

  if (field[3].length() == 0 ||
      field[4].length() == 0 ||
      field[5].length() == 0 ||
      field[6].length() == 0)
  {
    gpsFix = false;
    return false;
  }

  latitude = nmeaToDecimal(field[3], field[4][0]);
  longitude = nmeaToDecimal(field[5], field[6][0]);

  double knots = field[7].toDouble();
  speedKmph = knots * 1.852;

  utcDate = field[9];

  gpsFix = true;
  newGPSData = true;

  return true;
}

// ---------- Read GPS data from STM32 ----------
void readSTM32()
{
  while (STM32Serial.available())
  {
    char c = (char)STM32Serial.read();

    if (c == '\n')
    {
      nmeaLine.trim();

      if (nmeaLine.length() > 0)
      {
        Serial.print("NMEA: ");
        Serial.println(nmeaLine);
        parseRMC(nmeaLine);
      }

      nmeaLine = "";
    }
    else if (c != '\r')
    {
      if (nmeaLine.length() < 160)
        nmeaLine += c;
      else
        nmeaLine = "";
    }
  }
}

// ---------- Wi-Fi ----------
void connectWiFi()
{
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("Connecting to Wi-Fi...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < 15000)
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("Wi-Fi CONNECTED");
    Serial.print("ESP32 IP: ");
    Serial.println(WiFi.localIP());
  }
  else
  {
    Serial.println("Wi-Fi connection FAILED");
  }
}

// ---------- Time formatting ----------
String formatTime(String value)
{
  if (value.length() < 6) return value;

  return value.substring(0,2) + ":" +
         value.substring(2,4) + ":" +
         value.substring(4,6);
}

String formatDate(String value)
{
  if (value.length() < 6) return value;

  return value.substring(0,2) + "/" +
         value.substring(2,4) + "/20" +
         value.substring(4,6);
}

// ---------- Send JSON to local server ----------
void sendGPS()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("Wi-Fi disconnected. Data not sent.");
    return;
  }

  if (!gpsFix)
  {
    Serial.println("No GPS fix. Data not sent.");
    return;
  }

  HTTPClient http;

  String url = "http://" + String(SERVER_IP) +
               ":" + String(SERVER_PORT) +
               String(SERVER_PATH);

  Serial.print("Server URL: ");
  Serial.println(url);

  if (!http.begin(url))
  {
    Serial.println("HTTP begin failed.");
    return;
  }

  http.addHeader("Content-Type", "application/json");

  String json = "{";
  json += "\"latitude\":" + String(latitude, 6) + ",";
  json += "\"longitude\":" + String(longitude, 6) + ",";
  json += "\"speed_kmph\":" + String(speedKmph, 2) + ",";
  json += "\"utc_time\":\"" + formatTime(utcTime) + "\",";
  json += "\"utc_date\":\"" + formatDate(utcDate) + "\",";
  json += "\"gps_status\":\"FIXED\",";
  json += "\"esp32_ip\":\"" + WiFi.localIP().toString() + "\"";
  json += "}";

  Serial.print("Sending: ");
  Serial.println(json);

  int code = http.POST(json);

  if (code > 0)
  {
    Serial.print("HTTP response: ");
    Serial.println(code);
    Serial.print("Server: ");
    Serial.println(http.getString());
  }
  else
  {
    Serial.print("HTTP error: ");
    Serial.println(http.errorToString(code));
  }

  http.end();
}

// ---------- Status ----------
void printStatus()
{
  Serial.println();
  Serial.println("========== STATUS ==========");

  Serial.print("GPS FIX: ");
  Serial.println(gpsFix ? "YES" : "NO");

  if (gpsFix)
  {
    Serial.print("Latitude : ");
    Serial.println(latitude, 6);

    Serial.print("Longitude: ");
    Serial.println(longitude, 6);

    Serial.print("Speed   : ");
    Serial.print(speedKmph, 2);
    Serial.println(" km/h");

    Serial.print("UTC Time: ");
    Serial.println(formatTime(utcTime));

    Serial.print("UTC Date: ");
    Serial.println(formatDate(utcDate));
  }

  Serial.print("Wi-Fi: ");
  Serial.println(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");

  Serial.println("============================");
}

// ---------- SETUP ----------
void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("======================================");
  Serial.println(" REAL-TIME GPS VEHICLE TRACKING");
  Serial.println(" STM32 + ESP32 + LOCAL SERVER");
  Serial.println("======================================");

  // UART2: ESP32 receives NMEA from STM32
  STM32Serial.begin(9600, SERIAL_8N1, STM32_RX, STM32_TX);

  Serial.println("UART2 started at 9600 baud.");

  connectWiFi();
}

// ---------- LOOP ----------
void loop()
{
  readSTM32();

  if (millis() - lastWiFiCheck >= WIFI_CHECK_INTERVAL)
  {
    lastWiFiCheck = millis();

    if (WiFi.status() != WL_CONNECTED)
      connectWiFi();
  }

  if (newGPSData &&
      millis() - lastSend >= SEND_INTERVAL)
  {
    lastSend = millis();

    sendGPS();
    printStatus();

    newGPSData = false;
  }

  delay(5);
}
