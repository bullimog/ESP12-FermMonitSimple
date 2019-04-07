//FOR ESP-12E Module

//Network includes
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>

#include <EEPROM.h>

//Temp Sensor includes
#include <OneWire.h>
#include <DallasTemperature.h>

//LCD Display
#include <Wire.h>  // Comes with Arduino IDE
#include <LiquidCrystal_I2C.h> // https://bitbucket.org/fmalpartida/new-liquidcrystal/wiki/Home

LiquidCrystal_I2C lcd(0x3F, 2, 1, 0, 4, 5, 6, 7, 3, POSITIVE);  // Set the LCD I2C address


//Temp sensor defs
#define ONE_WIRE_BUS 2
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
DeviceAddress wortProbe = { 0x28, 0xFF, 0x86, 0x58, 0x22, 0x17, 0x03, 0x60 }; 
DeviceAddress fridgeProbe = { 0x28, 0xFF, 0x11, 0x5A, 0x22, 0x17, 0x03, 0x81 }; 
DeviceAddress shedProbe = { 0x28, 0xFF, 0x67, 0x37, 0x22, 0x17, 0x03, 0x7D }; 

//I/O Pin defs
const byte interruptPin = 14;  // GPIO14=D5
const byte heatingPin = 12;  // GPIO12=D6
const byte coolingPin = 13;  // GPIO13=D7

//Log defs
const int histSize = 200;
const int tempCount = 3;


const byte WORT = 0;
const byte FRIDGE = 1;
const byte SHED = 2;
const byte BUBBLE = 3;
const byte INDEX = 4;
const byte HEATCOOL = 5;
const String descs[]={"Wort", "Fridge", "Shed", "Bubbles", "T+", "Heat/Cool"};

const unsigned long bubbleDuration =  300; //ms between counting bubbles
unsigned long lastBubble = 0;

const unsigned long monitorDuration =  3000; // ms between lcd updates
unsigned long historyDuration =  900000; // ms between records
const unsigned long MIN_HISTORY_DURATION = 5000;
const int HIST_DURATION_EEPROM_POSN = 8;
float mins = (float)(historyDuration/60000.0);
float hrs = (float)(mins/60.0);

float temps[tempCount][histSize];
int bubbles[histSize];
byte heatCool[histSize]; //cool & heat history bit0=cool; bit1=heat
const byte COOL = 1;
const byte HEAT = 2;
boolean histFull = false;
int timeSeq = 0;

int bubbleCount = 0;
boolean heating = false;
boolean cooling = false;


unsigned long lastHistory = 0;
unsigned long lastMonitor = 0;
int logIdx = 0;

float target = 18.0;
float tolerance = 0.5;

float wortCalibrate = 0.0;
float fridgeCalibrate = 0.0;
float shedCalibrate = 0.0;

const int TARGET_EEPROM_POSN = 0;
const int TOLERANCE_EEPROM_POSN = 4;
const int WORT_CALIB_EEPROM_POSN = 12;
const int FRIDGE_CALIB_EEPROM_POSN = 16;
const int SHED_CALIB_EEPROM_POSN = 20;


ESP8266WebServer server(80);

float getEepromFloat(int posn){
  float val;
  EEPROM.get(posn, val);
  if(val == NAN){val = 0.0;}
  return val;  
}


void setup(void)
{
  lcd.begin(16,2);
  lcd.backlight();
  lcd.setCursor(0,0);
  lcd.print("Starting...");
  
  delay(1000);
  Serial.begin(115200);
  Serial.print("\n\r \n\r Started...");

  // Connect to WiFi network
  Serial.print("\n\r \n\r Connecting with WiFiManager...");
  WiFi.hostname("Fermenter");
  WiFiManager wifiManager;
  wifiManager.setDebugOutput(true);
  wifiManager.autoConnect("FermentMonitor");
  Serial.print("\n\r \n\r WiFi Connected...");

  String str = "ESP Web Server Connected to IP address: ";
  Serial.println("");
  Serial.print(str);
  Serial.println(WiFi.localIP());

  lcd.setCursor(0,0);
  lcd.print("IP:");
  lcd.print(WiFi.localIP());
  delay(1000);

  EEPROM.begin(1024);
  
  unsigned long _historyDuration;
  EEPROM.get(HIST_DURATION_EEPROM_POSN, _historyDuration);
  if(_historyDuration != NAN){historyDuration = _historyDuration;}

  target =          getEepromFloat(TARGET_EEPROM_POSN);
  tolerance =       getEepromFloat(TOLERANCE_EEPROM_POSN);
  wortCalibrate =   getEepromFloat(WORT_CALIB_EEPROM_POSN);
  fridgeCalibrate = getEepromFloat(FRIDGE_CALIB_EEPROM_POSN);
  shedCalibrate =   getEepromFloat(SHED_CALIB_EEPROM_POSN);

  server.begin();
  Serial.println("HTTP server started");
  sensors.begin();
  // 9 bits  0.5°C 93.75 ms; 10 bits 0.25°C  187.5 ms; 11 bits 0.125°C 375 ms; 12 bits   0.0625°C  750 ms
  sensors.setResolution(wortProbe, 12); //resolution 9 to 12 bits
  sensors.setResolution(fridgeProbe, 12);
  sensors.setResolution(shedProbe, 12);

  Serial.print("Initializing bubble sensor...");
  pinMode(interruptPin, INPUT);   //INPUT_PULLUP
  attachInterrupt(digitalPinToInterrupt(interruptPin), bubbleChanged, RISING);

  Serial.print("Initializing heat/cool pins...");
  pinMode(heatingPin, OUTPUT);
  pinMode(coolingPin, OUTPUT);
 
  server.on("/", HTTP_GET, handle_root);
  server.on("/clear", HTTP_GET, clearHistory);
  server.on("/hist", HTTP_GET, history);
  server.on("/config", HTTP_GET, configure);
  server.on("/configure", HTTP_GET, configureForm);
  server.on("/test", HTTP_GET, [](){
    String webString="tested okay...: ";
    server.send(200, "text/html", webString);
  });

  server.onNotFound(handleNotFound);
}

void handleNotFound(){
  String webString="Not found...: ";
  server.send(404, "text/html", webString);
}
 
void loop(void)
{
  server.handleClient();
  saveHistory();
  monitorTemperature();
}

void monitorTemperature(){
  if (millis() > (lastMonitor + monitorDuration)) {
    sensors.requestTemperatures();
    float wortTemp = sensors.getTempC(wortProbe) + wortCalibrate;
    float fridgeTemp = sensors.getTempC(fridgeProbe) + fridgeCalibrate;
    float shedTemp = sensors.getTempC(shedProbe) + shedCalibrate;

    if(wortTemp > target + tolerance){
      digitalWrite(coolingPin, HIGH);
      cooling = true;
    }
    if(wortTemp <= target){
      digitalWrite(coolingPin, LOW);
      cooling = false;
    }
    if(wortTemp < target - tolerance){
      digitalWrite(heatingPin, HIGH);
      heating = true;
    }
    if(wortTemp >= target){
      digitalWrite(heatingPin, LOW);
      heating = false;
    }

    if(cooling){
      heatCool[logIdx] = heatCool[logIdx] | COOL;
    }
    if(heating){
      heatCool[logIdx] = heatCool[logIdx] | HEAT;
    }
    
    updateDisplay(wortTemp, fridgeTemp, shedTemp);
    lastMonitor = millis();
  }
}

void updateDisplay(float wortTemp, float fridgeTemp, float shedTemp){
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print(oneDecPlace(wortTemp));

  if(heating){
    lcd.setCursor(9,0);
    lcd.print("Heating");
  }
  else if(cooling){
    lcd.setCursor(9,0);
    lcd.print("Cooling");
  }
 
  lcd.setCursor(0,1);
  lcd.print(oneDecPlace(fridgeTemp));
  lcd.setCursor(7,1);
  lcd.print(bubbleCount);
  lcd.setCursor(12,1);
  lcd.print(oneDecPlace(shedTemp));
}


//interrupt service routine
void bubbleChanged() {
      if (millis() > (lastBubble + bubbleDuration)) {
      bubbleCount ++;
      lastBubble = millis();
    }
}

void configureForm(){
  String str = "<html><head><title>Configure Fermentation Monitor</title></head>";
  str += "<body><form action=\"config\" method=\"get\">";

  str += "Target: <input type=\"number\" step=\"0.1\" maxlength=\"5\" name=\"target\" value=\"";
  str += (String) target;
  str += "\">&#176;C<br><br>";

  str += "Tolerance: <input type=\"number\" step=\"0.1\" maxlength=\"5\" name=\"tolerance\" value=\"";
  str += (String) tolerance;
  str += "\">&#176;C<br><br>";

  str += "History duration: <input type=\"number\" maxlength=\"4\"  name=\"history-duration\" value=\"";
  str += (String) (historyDuration/1000);
  str += "\"> seconds<br><br>";
  
  str += "Wort calibrate: <input type=\"number\" step=\"0.01\" maxlength=\"4\"  name=\"wort-calibrate\" value=\"";
  str += (String) wortCalibrate;
  str += "\">&#176;C<br><br>";
  
  str += "Fridge calibrate: <input type=\"number\" step=\"0.01\" maxlength=\"4\"  name=\"fridge-calibrate\" value=\"";
  str += (String) fridgeCalibrate;
  str += "\">&#176;C<br><br>";
  
  str += "Shed calibrate: <input type=\"number\" step=\"0.01\" maxlength=\"4\"  name=\"shed-calibrate\" value=\"";
  str += (String) shedCalibrate;
  str += "\">&#176;C<br><br>";
  
  str += "Write to EEPROM? ";
  str += "<input type=\"checkbox\" name=\"eeprom\" value=\"1\">Yes<br><br>";

  str += "<input type=\"submit\" value=\"Submit\"> &nbsp; &nbsp;";
  str += "<a href=\"hist\">Cancel</a>";
  str += "</form></head></html>";
  server.send(200, "text/html", str);
}

void configure(){
  float newTarget = server.arg("target").toFloat();
  float newTolerance = server.arg("tolerance").toFloat();
  int newHd = server.arg("history-duration").toInt();
  unsigned long newHistoryDuration= newHd * 1000;
  float newWortCalibrate = server.arg("wort-calibrate").toFloat();
  float newFridgeCalibrate = server.arg("fridge-calibrate").toFloat();
  float newShedCalibrate = server.arg("shed-calibrate").toFloat();
  String eeprom = server.arg("eeprom");

  if(newTarget > 0.0){target = newTarget;}
  if(newTolerance > 0.0){tolerance = newTolerance;}

  if((newWortCalibrate > -50.0) && (newWortCalibrate < 50.0)){
    wortCalibrate = newWortCalibrate;
  }
  if((newFridgeCalibrate > -50.0) && (newFridgeCalibrate < 50.0)){
    fridgeCalibrate = newFridgeCalibrate;
  }
  if((newShedCalibrate > -50.0) && (newShedCalibrate < 50.0)){
    shedCalibrate = newShedCalibrate;
  }

  if(newHistoryDuration >= MIN_HISTORY_DURATION){
    historyDuration = newHistoryDuration;
    mins = (float)(historyDuration/60000.0);
    hrs = (float)(mins/60.0);
  }

  if(eeprom.length() > 0){
    Serial.println("EEPROM was set");
    //write config to eeprom
    EEPROM.put(HIST_DURATION_EEPROM_POSN, historyDuration);
    EEPROM.put(TARGET_EEPROM_POSN, target);
    EEPROM.put(TOLERANCE_EEPROM_POSN, tolerance);
    EEPROM.put(WORT_CALIB_EEPROM_POSN, wortCalibrate);
    EEPROM.put(FRIDGE_CALIB_EEPROM_POSN, fridgeCalibrate);
    EEPROM.put(SHED_CALIB_EEPROM_POSN, shedCalibrate);
    EEPROM.commit();
  }

  server.sendHeader("Location","/hist"); 
  server.send(303);
}

void clearHistory() {

  String webString="";

  Serial.print("Deleting temperature history ");

  for(int i=0; i< histSize; i++){
    bubbles[i] = 0;
    heatCool[i] = 0;
    for(int t=0; t<tempCount; t++){
      temps[t][i] = 0;
    }
  }

  webString+="Initialised data<br>";
  logIdx = 0;
  histFull = false;
  server.send(200, "text/html", webString);
}


void saveHistory() {
  if (millis() > (lastHistory + historyDuration)) {
    Serial.println("Saving history...");
    Serial.print("Log Index:");
    Serial.println(logIdx);
    bubbles[logIdx] = bubbleCount;
    bubbleCount = 0;

    sensors.requestTemperatures();
    float wortTemp = sensors.getTempC(wortProbe) + wortCalibrate;
    float fridgeTemp = sensors.getTempC(fridgeProbe) + fridgeCalibrate;
    float shedTemp = sensors.getTempC(shedProbe) + shedCalibrate;

    temps[WORT][logIdx] = wortTemp;
    temps[FRIDGE][logIdx] = fridgeTemp;
    temps[SHED][logIdx] = shedTemp;

    if(logIdx < histSize-1){
      logIdx++;
    }
    else{
      logIdx = 0;
      histFull = true;
    }
    
    heatCool[logIdx] = 0;
    lastHistory = millis();
  }
}

void handle_root() {
  sensors.requestTemperatures();

  float wortTemp = sensors.getTempC(wortProbe) + wortCalibrate;
  float fridgeTemp = sensors.getTempC(fridgeProbe) + fridgeCalibrate;
  float shedTemp = sensors.getTempC(shedProbe) + shedCalibrate;
  
  unsigned long timeLeft = lastHistory + historyDuration - millis();
  
  String str = "<html><head><title>Current Fermenter Values</title><head><body><fieldset><legend>Temperatures</legend>Wort:";
  str += (String) wortTemp;
  str += "<br>Fridge: ";
  str += (String) fridgeTemp;
  str += "<br>Shed: ";
  str += (String) shedTemp;
  str += "</fieldset>";
  str += "<br>Bubble Count:";
  str += (String) bubbleCount;
  str += "<br>Cooling:";
  str += (String) cooling;
  str += "<br>Heating:";
  str += (String) heating;
  str += "<br>Next reading in:";
  str += (String) (timeLeft/1000);
  str += " seconds<br><br>";
  str += "<a href=\"hist\">History</a> &nbsp; &nbsp;";
  str += "<a href=\"configure\">Configure</a>";
  str += "</body></html>";
  server.send(200, "text/html", str);
}


void history(){

  int len = 0;
  String head = "<html><head><title>Fermentation Monitor History</title><style>";
  head += "table {border-collapse: collapse;} table, th, td {padding: 0px; border: 1px solid black;}";
  head += "</style></head><table>";
  String foot = "</table><br>Target: ";
  foot += target;
  foot += "<br>Tolerance: ";
  foot += tolerance;
  foot += "<br>History duration: ";
  foot += (historyDuration/1000);
  foot += " seconds<br><br>";
  foot += "<a href=\"configure\">Configure</a> &nbsp; &nbsp;";
  foot += "<a href=\"/\">Current</a>";
  foot += "</html>";
  len += head.length();
  len += tableRow("th", INDEX, false);
  len += tableRow("td", WORT, false);
  len += tableRow("td", FRIDGE, false);
  len += tableRow("td", SHED, false);
  len += tableRow("td", HEATCOOL, false);
  len += tableRow("td", BUBBLE, false);
  len += foot.length();

  server.setContentLength(len);
  server.send(200, "text/html", ""); 

  server.sendContent(head);
  tableRow("th", INDEX, true);
  tableRow("td", WORT, true);
  tableRow("td", FRIDGE, true);
  tableRow("td", SHED, true);
  tableRow("td", HEATCOOL, true);
  tableRow("td", BUBBLE, true);
  server.sendContent(foot);

  Serial.print("Bytes Sent:");
  Serial.println(len);
}

int tableRow(String tag, int rowMode, boolean sending){
  String str = "<tr><th>";
  str += descs[rowMode];
  str += "</th>";
  int contentLen = str.length();
  if(sending){ server.sendContent(str); }

  int first = 0;
  int last = logIdx-1;
  timeSeq = logIdx;
  if(histFull==true){
    first = logIdx;
    last = histSize-1;
    timeSeq = histSize;
  }

  contentLen += tableCells(first, last, tag, rowMode, sending);

  if(histFull){
    last = logIdx-1;
    contentLen += tableCells(0, last, tag, rowMode, sending);
  }

  str = "</tr>\n";  
  contentLen += str.length();
  if(sending){ server.sendContent(str); }

  return contentLen;
}


int tableCells(int first, int last, String tag, int rowMode, boolean sending){
  String str = "";
  int contentLen = 0;

  if(last < 0){ last = 0;}

  for(int i=first; i<= last; i++){
    str = "<";
    str += tag;
    if(rowMode < 3){
      str += " bgcolor=\"";
      str += colour(temps[rowMode] [i]);
      str += "\"";
    }
    else if(rowMode == HEATCOOL){
      str += " bgcolor=\"";
      if(heatCool[i] & HEAT){str+="FF";}
      else{str+="00";}
      str += "00";
      if(heatCool[i] & COOL){str+="FF";}
      else{str+="00";}
      str += "\"";
    }
    
    str += ">";
    contentLen += str.length();
    if(sending){ server.sendContent(str); }
    contentLen += tableCell(i, rowMode, sending);
    
    str = "</";
    str += tag;
    str += ">";
    contentLen += str.length();
    if(sending){ server.sendContent(str); }
  }
  return contentLen;
}


String formatTime(int t){
  float h = (float)t * hrs;
  byte bh = (byte)h;
  float m = ((float)t * mins) - (bh * 60.0);
  byte bm = (byte)m;
  String str = pad((String)bh);
  str +=":";
  str += pad((String)bm);
  return str;
}

int tableCell(int idx, int rowMode, boolean sending){
  String str = "";
  switch(rowMode){
    case INDEX:
      str = formatTime(timeSeq);
      timeSeq --;
      break;
    case BUBBLE:
      str = (String)bubbles[idx]; 
      break;
    case HEATCOOL:
      if(heatCool[idx] == HEAT) str = "Heat";
      if(heatCool[idx] == COOL) str = "Cool";
      if(heatCool[idx] == (HEAT | COOL)) str = "H&amp;C";
      break;
    default:
      int a = round(temps[rowMode] [idx] * 10);
      float b = a / 10.0;
      str = (String)b;
      str = str.substring(0, str.length()-1);
      colour(b);
  }
  if(sending){
    server.sendContent(str);
  }

  return str.length();
}

String colour(float reading){

  float redOffset = reading - target;
  if(redOffset < 0) {redOffset = 0;}
  int red  = (int) (redOffset / tolerance * 255);
  if (red > 255) {red = 255;}

  float blueOffset = target - reading;
  if(blueOffset < 0) {blueOffset = 0;}
  int blue  = (int) (blueOffset / tolerance * 255);
  if (blue > 255) {blue = 255;}

  int green = 255 - red - blue;

  String output = "#";
  output += pad(String(red, HEX));
  output += pad(String(green, HEX));
  output += pad(String(blue, HEX));

  return output;
}

String pad(String num){
  if(num.length()<2){
    return "0" + num;
  }
  return num;
}

String oneDecPlace(float reading){
  int a = round(reading * 10);
  float b = a / 10.0;
  String str = (String)b;
  return str.substring(0, str.length()-1);
}
