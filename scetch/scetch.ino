#include <FileData.h>
#include <SPIFFS.h>

#include <WebServer.h>
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>

#include <OneWire.h>
#include <DallasTemperature.h>

#include <microDS3231.h>

#include <GyverHTU21D.h>

#define DBG_OUTPUT_PORT Serial
#define light_quantity 6  // Количество подключенных реле на освещение


// Настройка для датчика кислотности ph
#define OPERATING_VOLTAGE   3.3
#define ZERO_SHIFT          1.1

// Коэффициент перевода напряжения в концентрацию pH
#define CALIBRATION_FACTOR  3.5

struct Data {
  uint8_t alarmTime[light_quantity][5];
};
Data mydata;

FileData data(&SPIFFS, "/data.dat", 'B', &mydata, sizeof(mydata));

GyverHTU21D htu;    // htu21d объект влажности и температуры

const byte phSensor_1 = 34;
const byte phSensor_2 = 35;

int latchPin = 4;   // Pin ST_CP на 74HC595
int clockPin = 12;  // Pin SH_CP на 74HC595
int dataPin = 14;   // Pin DS на 74HC595

byte SR2LEDS = 0;  // Переменная для второй 74HC595
byte SR1LEDS = 0;  // Переменная для второй 74HC595

const char *ssid = "wifiESP";  // имя точки доступа, которую будет раздавать ESP
const char *password = "12345678";

MicroDS3231 rtc;  // модуль реального времени

OneWire oneWire(32);  // датчик температуры
DallasTemperature ds(&oneWire);
byte num;

WebServer server(80);  // порт работы

static bool hasSD = false;
File uploadFile;


byte alarmState[light_quantity][2] = {  // флаг о том, что будильник сегодня уже включался / выключался соответственно
  { 1, 1 },
  { 1, 1 },
  { 1, 1 },
  { 1, 1 },
  { 1, 1 },
  { 1, 1 }
};



void checkAlarm() {
  DateTime now = rtc.getTime();
  for (int i = 0; i < light_quantity; i++) {
    if (mydata.alarmTime[i][0] and !cur_output(i, 2).toInt() and alarmState[i][0]) {
      if ((now.hour >= mydata.alarmTime[i][1]) and (now.minute >= mydata.alarmTime[i][2])) {
        handleSR74HC595_2(i, 1);
        alarmState[i][0] = 0;
        Serial.println("on");
      }
    }

    if (mydata.alarmTime[i][0] and cur_output(i, 2).toInt() and alarmState[i][1]) {
      if ((now.hour >= mydata.alarmTime[i][3]) and (now.minute >= mydata.alarmTime[i][4])) {
        handleSR74HC595_2(i, 0);
        alarmState[i][1] = 0;
        Serial.println("off");
      }
    }
    if (now.hour == 0 and now.minute == 0) {
      alarmState[i][0] = 1;
      alarmState[i][1] = 1;
    }
  }
}

void returnOK() {
  server.send(200, "text/plain", "");
}

void returnFail(String msg) {
  server.send(500, "text/plain", msg + "\r\n");
}

bool loadFromSdCard(String path) {
  String dataType = "text/plain";
  if (path.endsWith("/")) path += "index.html";

  if (path.endsWith(".src")) path = path.substring(0, path.lastIndexOf("."));
  else if (path.endsWith(".html")) dataType = "text/html";
  else if (path.endsWith(".css")) dataType = "text/css";
  else if (path.endsWith(".js")) dataType = "application/javascript";
  else if (path.endsWith(".png")) dataType = "image/png";
  else if (path.endsWith(".gif")) dataType = "image/gif";
  else if (path.endsWith(".jpg")) dataType = "image/jpeg";
  else if (path.endsWith(".ico")) dataType = "image/x-icon";
  else if (path.endsWith(".xml")) dataType = "text/xml";
  else if (path.endsWith(".pdf")) dataType = "application/pdf";
  else if (path.endsWith(".zip")) dataType = "application/zip";

  File dataFile = SD.open(path.c_str());
  if (dataFile.isDirectory()) {
    path += "/index.html";
    dataType = "text/html";
    dataFile = SD.open(path.c_str());
  }

  if (!dataFile)
    return false;

  if (server.hasArg("download")) dataType = "application/octet-stream";

  if (server.streamFile(dataFile, dataType) != dataFile.size()) {
    DBG_OUTPUT_PORT.println("Sent less data than expected!");
  }

  dataFile.close();
  return true;
}

void handleFileUpload() {
  if (server.uri() != "/edit") return;
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    if (SD.exists((char *)upload.filename.c_str())) SD.remove((char *)upload.filename.c_str());
    uploadFile = SD.open(upload.filename.c_str(), FILE_WRITE);
    DBG_OUTPUT_PORT.print("Upload: START, filename: ");
    DBG_OUTPUT_PORT.println(upload.filename);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
    DBG_OUTPUT_PORT.print("Upload: WRITE, Bytes: ");
    DBG_OUTPUT_PORT.println(upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
    DBG_OUTPUT_PORT.print("Upload: END, Size: ");
    DBG_OUTPUT_PORT.println(upload.totalSize);
  }
}

void deleteRecursive(String path) {
  File file = SD.open((char *)path.c_str());
  if (!file.isDirectory()) {
    file.close();
    SD.remove((char *)path.c_str());
    return;
  }

  file.rewindDirectory();
  while (true) {
    File entry = file.openNextFile();
    if (!entry) break;
    String entryPath = path + "/" + entry.name();
    if (entry.isDirectory()) {
      entry.close();
      deleteRecursive(entryPath);
    } else {
      entry.close();
      SD.remove((char *)entryPath.c_str());
    }
    yield();
  }

  SD.rmdir((char *)path.c_str());
  file.close();
}

void handleDelete() {
  if (server.args() == 0) return returnFail("BAD ARGS");
  String path = server.arg(0);
  if (path == "/" || !SD.exists((char *)path.c_str())) {
    returnFail("BAD PATH");
    return;
  }
  deleteRecursive(path);
  returnOK();
}

void handleCreate() {
  if (server.args() == 0) return returnFail("BAD ARGS");
  String path = server.arg(0);
  if (path == "/" || SD.exists((char *)path.c_str())) {
    returnFail("BAD PATH");
    return;
  }

  if (path.indexOf('.') > 0) {
    File file = SD.open((char *)path.c_str(), FILE_WRITE);
    if (file) {
      // file.write((const char *)0, 1);  // дичь какая-то
      file.write(0);
      file.close();
    }
  } else {
    SD.mkdir((char *)path.c_str());
  }
  returnOK();
}

void printDirectory() {
  if (!server.hasArg("dir")) return returnFail("BAD ARGS");
  String path = server.arg("dir");
  if (path != "/" && !SD.exists((char *)path.c_str())) return returnFail("BAD PATH");
  File dir = SD.open((char *)path.c_str());
  path = String();
  if (!dir.isDirectory()) {
    dir.close();
    return returnFail("NOT DIR");
  }
  dir.rewindDirectory();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/json", "");
  WiFiClient client = server.client();

  server.sendContent("[");
  for (int cnt = 0; true; ++cnt) {
    File entry = dir.openNextFile();
    if (!entry)
      break;

    String output;
    if (cnt > 0)
      output = ',';

    output += "{\"type\":\"";
    output += (entry.isDirectory()) ? "dir" : "file";
    output += "\",\"name\":\"";
    output += entry.name();
    output += "\"";
    output += "}";
    server.sendContent(output);
    entry.close();
  }
  server.sendContent("]");
  dir.close();
}

void handleNotFound() {
  if (hasSD && loadFromSdCard(server.uri())) return;
  String message = "SDCARD Not Detected\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " NAME:" + server.argName(i) + "\n VALUE:" + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
  DBG_OUTPUT_PORT.print(message);
}

void relay_switch() {
  int relay_num = (server.arg("val")).toInt();  // Получаем значение ssdp из запроса сохраняем в глобальной переменной
  byte state;
  byte HC595_num;
  if (relay_num >= 10 && relay_num < 20) {
    relay_num -= 10;
    HC595_num = 1;
  } else if (relay_num >= 20 && relay_num < 30) {
    relay_num -= 20;
    HC595_num = 2;
  }

  if (cur_output(relay_num, HC595_num).toInt()) {
    // Serial.print("текущее состояние выхода: ");
    // Serial.println(cur_output(relay_num, HC595_num));
    state = 0;
  } else {
    // Serial.print("текущее состояние выхода: ");
    // Serial.println(cur_output(relay_num, HC595_num));
    state = 1;
  }

  if (HC595_num == 1) {
    handleSR74HC595_1(relay_num, state);
  } else {
    handleSR74HC595_2(relay_num, state);
  }
  server.send(200, "text/plain", String(state));  // отправляем ответ о выполнении
}

void relay_status() {
  int relay_num = (server.arg("val")).toInt();  // Получаем значение relay ID из запроса сохраняем в переменной
  byte state;
  byte HC595_num;
  if (relay_num >= 10 && relay_num < 20) {
    relay_num -= 10;
    HC595_num = 1;
  } else if (relay_num >= 20 && relay_num < 30) {
    relay_num -= 20;
    HC595_num = 2;
  }
  if (cur_output(relay_num, HC595_num).toInt()) {
    // Serial.print("текущее состояние выхода: ");
    // Serial.println(cur_output(relay_num, HC595_num));
    state = 1;
  } else {
    // Serial.print("текущее состояние выхода: ");
    // Serial.println(cur_output(relay_num, HC595_num));
    state = 0;
  }
  server.send(200, "text/plain", String(state));  // отправляем ответ о выполнении
}

void set_alarm() {
  int light_id = (server.arg("val")).toInt();     // Получаем значение val из запроса и сохраняем в переменной
  String light_time_on = server.arg("timeOn");    // Получаем значение временя включения из запроса и сохраняем в переменной
  String light_time_off = server.arg("timeOff");  // Получаем значение времени выключения из запроса и сохраняем в переменной
  mydata.alarmTime[light_id][0] = 1;
  alarmState[light_id][0] = 1;
  alarmState[light_id][1] = 1;
  mydata.alarmTime[light_id][1] = light_time_on.substring(0, 2).toInt();
  mydata.alarmTime[light_id][2] = light_time_on.substring(3).toInt();
  mydata.alarmTime[light_id][3] = light_time_off.substring(0, 2).toInt();
  mydata.alarmTime[light_id][4] = light_time_off.substring(3).toInt();

  data.updateNow();

  if (data.updateNow() == FD_WRITE) {
    Serial.println("Data updated!");
  }
  // Serial.println('timeeeeee');
  // Serial.println(mydata.alarmTime[light_id][3]);
  // Serial.println(light_time_on.substring(2));
  // Serial.println(light_time_off);

  String request = "";
  for (int i = 1; i < 5; i++) {
    request += mydata.alarmTime[light_id][i];
    if (i == 4) break;
    request += ":";
    // Serial.println(mydata.alarmTime[light_id][i]);
  }

  server.send(200, "text/plain", String(request));  // отправляем ответ о выполнении
}

void getCurrAlarm() {
  int light_id = (server.arg("val")).toInt();  // Получаем значение val из запроса и сохраняем в переменной
  String request = "";
  if (mydata.alarmTime[light_id][0] == 1) {
    for (int i = 1; i < 5; i++) {
      request += mydata.alarmTime[light_id][i];
      if (i == 4) break;
      request += ":";
    }
  } else {
    request += "0";
  }
  server.send(200, "text/plain", String(request));  // отправляем ответ о выполнении
}

void getCurrTime() {
  DateTime now = rtc.getTime();
  String request = "";
  request += (now.hour > 9) ? String(now.hour) : ("0" + String(now.hour));
  request += ":";
  request += (now.minute > 9) ? String(now.minute) : ("0" + String(now.minute));
  server.send(200, "text/plain", String(request));  // отправляем ответ о выполнении
}

void setCurrTime() {
  int new_hour = (server.arg("hour")).toInt();      // Получаем значение hour из запроса и сохраняем в переменной
  int new_minute = (server.arg("minute")).toInt();  // Получаем значение minute из запроса и сохраняем в переменной


  DateTime new_time;
  new_time.second = 0;
  new_time.minute = new_minute;
  new_time.hour = new_hour;

  rtc.setTime(new_time);  // загружаем в RTC

  getCurrTime();
}

void offAlarm() {
  int light_id = (server.arg("val")).toInt();  // Получаем значение val из запроса и сохраняем в переменной
  String request = "";
  mydata.alarmTime[light_id][0] = 0;
  server.send(200, "text/plain", String(mydata.alarmTime[light_id][0]));  // отправляем ответ о выполнении
}

void getHumidTemp() {
  String request = "";
  if (htu.readTick()) {
    request += String(htu.getTemperature());
    request += ":";
    request += String(htu.getHumidity());
    server.send(200, "text/plain", String(request)); // отправляем ответ о выполнении
  } else {
    server.send(200, "text/plain", "Error!"); // отправляем ответ о выполнении
  }
}

void getPh() {
  int sensor_id = (server.arg("val")).toInt();  // Получаем значение val из запроса и сохраняем в переменной
  Serial.println(sensor_id);
  // int adcSensor;

  // if (sensor_id == 1) {
  //   adcSensor = analogRead(phSensor_1);
  // } else if (sensor_id == 2) {
  //   adcSensor = analogRead(phSensor_2);
  // }
  // delay(50);
  // Serial.println(adcSensor);
  int adcSensor = (sensor_id == 1) ? analogRead(phSensor_1): (sensor_id == 2) ? analogRead(phSensor_2): 0;
  float voltageSensor = (adcSensor * OPERATING_VOLTAGE / 4095) + 0.13;
  float  phValue = CALIBRATION_FACTOR * (voltageSensor + ZERO_SHIFT);
  Serial.println(phValue);
  server.send(200, "text/plain", String(phValue));  // отправляем ответ о выполнении
}

String check_temp() {
  ds.requestTemperatures();
  return String(ds.getTempCByIndex(0));
}

void handleSR74HC595_2(int relay_num, byte state) {  // Вторая 74HC595
  // String leds_state = String(relay_num);                         // Переменная для хранения данных с веб-страницы.
  byte SRPO, SRPS;  // Переменная byte для хранения данных

  SRPO = relay_num;  // SRPO берет данные из переменной leds_state
  SRPS = state;      // SRPS берет данные из переменной leds_state

  bitWrite(SR2LEDS, SRPO, SRPS);  // Запись битов Пример: bitWrite(SR2LEDS, SRPO, SRPS); = bitWrite(11111111, 7, 0); the result is SR2LEDS = 01111111

  updateShiftRegister(SR1LEDS, SR2LEDS);  // ввод/обновления/отправки байтов данных в 74HC595.

  Serial.print("SR1=");
  printBinaryByte(SR1LEDS);
  Serial.print(" ");
  Serial.print("SR2=");
  printBinaryByte(SR2LEDS);
  Serial.println();

  // server.send(200, "text/plane", cur_output());                             // Вывод страницы
}
// Первая 74HC595
void handleSR74HC595_1(int relay_num, byte state) {
  byte SRPO, SRPS;

  SRPO = relay_num;
  SRPS = state;

  bitWrite(SR1LEDS, SRPO, SRPS);

  updateShiftRegister(SR1LEDS, SR2LEDS);

  Serial.print("SR1=");
  printBinaryByte(SR1LEDS);
  Serial.print(" ");
  Serial.print("SR2=");
  printBinaryByte(SR2LEDS);
  Serial.println();

  // server.send(200, "text/plane", cur_output());
}

void printBinaryByte(byte value) {
  for (byte mask = 0x80; mask; mask >>= 1) {
    Serial.print((mask & value) ? '1' : '0');
  }
}

void updateShiftRegister(byte SR1, byte SR2) {
  digitalWrite(latchPin, LOW);
  shiftOut(dataPin, clockPin, MSBFIRST, SR2);
  shiftOut(dataPin, clockPin, MSBFIRST, SR1);
  digitalWrite(latchPin, HIGH);
}

String cur_output() {
  String cur_byte;
  cur_byte.reserve(20);
  cur_byte.clear();
  for (byte mask = 0x80; mask; mask >>= 1) {
    cur_byte += (mask & SR1LEDS) ? '1' : '0';
  }
  for (byte mask = 0x80; mask; mask >>= 1) {
    cur_byte += (mask & SR2LEDS) ? '1' : '0';
  }
  return cur_byte;
}

String cur_output(int n, int HC595_num) {
  String cur_byte;
  cur_byte.reserve(20);
  cur_byte.clear();
  if (HC595_num == 1) {
    for (byte mask = 0x80; mask; mask >>= 1) {
      cur_byte += (mask & SR1LEDS) ? '1' : '0';
    }
  } else {
    for (byte mask = 0x80; mask; mask >>= 1) {
      cur_byte += (mask & SR2LEDS) ? '1' : '0';
    }
  }
  // Serial.println(cur_byte);
  return String(cur_byte[7 - n]);
}



void setup(void) {
  pinMode(latchPin, OUTPUT);
  pinMode(dataPin, OUTPUT);
  pinMode(clockPin, OUTPUT);


  DBG_OUTPUT_PORT.begin(115200);
  DBG_OUTPUT_PORT.setDebugOutput(true);
  DBG_OUTPUT_PORT.print("\n");

  SPIFFS.begin(true);

  // прочитать данные из файла в переменную
  // при первом запуске в файл запишутся данные из структуры
  FDstat_t stat = data.read();

  switch (stat) {
    case FD_FS_ERR:
      Serial.println("FS Error");
      break;
    case FD_FILE_ERR:
      Serial.println("Error");
      break;
    case FD_WRITE:
      Serial.println("Data Write");
      break;
    case FD_ADD:
      Serial.println("Data Add");
      break;
    case FD_READ:
      Serial.println("Data Read");
      break;
    default:
      break;
  }

  ds.begin();
  num = ds.getDeviceCount();

  htu.begin();   // запустить датчик htu21

  WiFi.softAP(ssid, password);  // создаем точку доступа

  server.begin();  // инициализируем Web-server

  DBG_OUTPUT_PORT.print("\nMy IP to connect via Web-Browser or FTP: ");
  DBG_OUTPUT_PORT.println(WiFi.softAPIP());  // выводим локальный IP-адресс
  DBG_OUTPUT_PORT.println("\n");

  DBG_OUTPUT_PORT.print("Connected! IP address: ");
  DBG_OUTPUT_PORT.println(WiFi.localIP());

  updateShiftRegister(SR1LEDS, SR2LEDS);  // ввод/обновления/отправки байтов данных в 74HC595.


  server.on("/list", HTTP_GET, printDirectory);
  server.on("/edit", HTTP_DELETE, handleDelete);
  server.on("/edit", HTTP_PUT, handleCreate);
  server.on(
    "/edit", HTTP_POST, []() {
      returnOK();
    },
    handleFileUpload);
  server.on("/relay_switch", relay_switch);  // переключать состояние реле по запросу вида /relay_switch?val=relay_id
  server.on("/relay_status", relay_status);  // переключать состояние реле по запросу вида /relay_switch?val=relay_id
  server.on("/set_alarm", set_alarm);        // включать будильник по запросу вида /set_alarm?val=relay_id...
  server.on("/getCurrAlarm", getCurrAlarm);  // проверять состояние будильника (включен или выключен и время) по запросу вида /getCurrAlarm?val=...
  server.on("/getCurrTime", getCurrTime);    // отправлять текущее время по запросу вида /getCurrTime
  server.on("/setCurrTime", setCurrTime);    // отправлять текущее время по запросу вида /setCurrTime
  server.on("/offAlarm", offAlarm);    // отправлять текущее время по запросу вида /offAlarm?val=id
  server.on("/getHumidTemp", getHumidTemp);     // отправлять текущее время по запросу вида /getHumidTemp
  server.on("/getPh", getPh);  // проверять состояние будильника (включен или выключен и время) по запросу вида /getPh?val=...
  server.on("/check_temp", []() {
    server.send(200, "text/plain", check_temp());
  });
  server.onNotFound(handleNotFound);

  server.begin();
  DBG_OUTPUT_PORT.println("HTTP server started");

  htu.readTick();

  while (true) {
    if (SD.begin()) {
      DBG_OUTPUT_PORT.println("SD Card initialized.");
      hasSD = true;
      break;
    }
    delay(500);
    // DBG_OUTPUT_PORT.println("SD Card not initialized.");
  }
}



void loop(void) {
  server.handleClient();
  checkAlarm();
  if (data.tick() == FD_WRITE) Serial.println("Data updated!");
}
