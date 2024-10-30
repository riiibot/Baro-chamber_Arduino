#include <SPI.h>
#include <SD.h>
#include <WiFiNINA.h>
#include <WiFiUdp.h>
#include "arduino_secrets.h"

// Define the SD card chip select pin
const int chipSelect = 4;
File myFile;

char ssid[] = SECRET_SSID;
char pass[] = SECRET_PASS;
int status = WL_IDLE_STATUS;
WiFiServer server(80);

unsigned long lastToggleTime = 0;    
const unsigned long toggleDebounceDelay = 500; 

// NTP variables
WiFiUDP ntpUDP;
const char* ntpServer = "pool.ntp.org";
const int ntpPacketSize = 48;
byte packetBuffer[ntpPacketSize];
const unsigned long seventyYears = 2208988800UL;

const int pwmPin = 9;
const int baroChamberPin = A0;
const int controlChamberPin = A1;

float currentPWM = 15.0;
float currentPSI = 0.0;
unsigned long rampStartTime = 0;
bool ramping = false;
bool rampToTarget = false;
bool manualControlActive = false;
bool dataLoggingEnabled = false;

float rampStartPSI = 0.0;
float rampEndPSI = 15.0;
const unsigned long rampDuration = 7 * 60 * 60 * 1000UL;
const float psiIncrementPerMinute = 15.0 / 420.0;
const float rampToTargetIncrement = 0.035;

float targetPSI = 0.0;         // Desired PSI for ramp-to-target
bool isRampingToTarget = false; // Flag to track if ramp-to-target is active

const float pwmPercentages[] = {15, 17.5, 20, 22.5, 25, 27.5, 30, 32.5, 35, 37.5, 40, 42.5, 45, 47.5, 50, 52.5, 55, 57.5, 60, 62.5, 65, 67.5, 70, 72.5, 77.5, 80, 82.5, 85, 87.5, 90, 95, 100};
const float psiValues[] = {0, 0.3, 0.6, 0.8, 1.1, 1.4, 1.8, 2.1, 2.6, 3, 3.6, 4, 4.7, 5.1, 5.9, 6.4, 7.3, 7.8, 8.8, 9.4, 10.3, 10.9, 12.1, 12.9, 14.8, 16, 16.8, 18, 18.9, 20.4, 22.9, 22.9};
const int dataSize = sizeof(pwmPercentages) / sizeof(pwmPercentages[0]);

void setup() {
    Serial.begin(9600);
    pinMode(pwmPin, OUTPUT);
    pinMode(baroChamberPin, INPUT);
    pinMode(controlChamberPin, INPUT);

    if (!SD.begin(chipSelect)) {
        Serial.println("SD card initialization failed!");
        while (1);
    }
    myFile = SD.open("datalog.csv", FILE_WRITE);
    if (myFile) {
        myFile.println("Timestamp,Baro-Chamber PSI,Control Chamber PSI,PWM(%),Calculated PSI");
        myFile.close();
    }

    while (status != WL_CONNECTED) {
        status = WiFi.begin(ssid, pass);
        delay(10000);
    }
    server.begin();
    ntpUDP.begin(123);
}

void handleClientRequest(WiFiClient& client) {
    String currentLine = ""; // Stores the current line from the client request

    while (client.connected()) {
        if (client.available()) {
            char c = client.read();
            if (c == '\n') {
                // End of the client request header line
                if (currentLine.length() == 0) {
                    // Send the response to the client after receiving the full request
                    sendWebPage(client);
                    break;
                } else {
                    // Handle HTTP request if a full line is read
                    handleHttpRequest(currentLine);
                }
                currentLine = "";
            } else if (c != '\r') {
                currentLine += c; // Append character to the current line
            }
        }
    }
    client.stop(); // Close the connection with the client
}


void loop() {
    WiFiClient client = server.available();
    if (client) {
        handleClientRequest(client);
    }

    // Debugging: Monitor the ramping state before calling `updateRamping`
    Serial.print("isRampingToTarget: ");
    Serial.print(isRampingToTarget);
    Serial.print(" | ramping: ");
    Serial.print(ramping);
    Serial.print(" | currentPSI: ");
    Serial.print(currentPSI);
    Serial.print(" | currentPWM: ");
    Serial.println(currentPWM);

    if (dataLoggingEnabled) {
        logDataToSD();
    }

    // Update the ramping logic if either `ramping` or `isRampingToTarget` is active
    updateRamping();
}


void handleHttpRequest(String request) {
     if (request.startsWith("GET /toggleLogging")) {
        if (millis() - lastToggleTime > toggleDebounceDelay) {
            dataLoggingEnabled = !dataLoggingEnabled;
            Serial.print("Data logging: ");
            Serial.println(dataLoggingEnabled ? "Enabled" : "Disabled");
            lastToggleTime = millis();  // Update debounce timer
        }
    }
    
    // Start ramp to target PSI
    else if (request.startsWith("GET /rampToTarget/")) {
        // Parse target PSI from the request string
        int startIndex = request.indexOf("/rampToTarget/") + 14;
        int endIndex = request.indexOf(" ", startIndex);
        String targetStr = request.substring(startIndex, endIndex);
        targetPSI = targetStr.toFloat();

        // Confirm that targetPSI is valid before starting the ramp
        Serial.print("Parsed Target PSI: ");
        Serial.println(targetPSI);
        if (targetPSI <= 0.0) {
            Serial.println("Error: Target PSI not set correctly.");
            return;  // Exit if target PSI is invalid
        }

        // Set up ramp variables
        rampStartPSI = currentPSI;  // Start from the current PSI
        isRampingToTarget = true;   // Activate ramp-to-target mode
        ramping = false;
        manualControlActive = false;
        rampStartTime = millis();   // Record start time for ramp
    }
    
    // Handle other commands such as setting PWM or reinitializing SD
    else if (request.startsWith("GET /setPWM/")) {
        // Set PWM manually, cancelling any active ramp
        int index = request.indexOf("GET /setPWM/") + 12;
        String pwmStr = request.substring(index);
        pwmStr = pwmStr.substring(0, pwmStr.indexOf(' '));
        currentPWM = pwmStr.toFloat();
        currentPSI = calculatePSI(currentPWM);
        analogWrite(pwmPin, (int)(currentPWM / 100.0 * 255));
        manualControlActive = true;
        ramping = false;
        isRampingToTarget = false;  // Cancel ramp-to-target if manually controlled
    }
    
    else if (request.startsWith("GET /reinitializeSD")) {
        reinitializeSDCard();
    }
}




// Add your `logDataToSD` function here
void logDataToSD() {
    myFile = SD.open("datalog.csv", FILE_WRITE);
    if (myFile) {
        String timestamp = getNTPTime();
        myFile.print(timestamp);
        myFile.print(",");
        myFile.print(readBaroChamberPSI());
        myFile.print(",");
        myFile.print(readControlChamberPSI());
        myFile.print(",");
        myFile.print(currentPWM);
        myFile.print(",");
        myFile.println(currentPSI);
        myFile.close();
        Serial.println("Data logged to SD card.");
    } else {
        Serial.println("Error writing to SD card.");
    }
}


void reinitializeSDCard() {
    Serial.println("Reinitializing SD card...");

    if (!SD.begin(chipSelect)) {
        Serial.println("SD card initialization failed!");
        return;
    }

    // Recreate the log file
    myFile = SD.open("datalog.csv", FILE_WRITE);
    if (myFile) {
        myFile.println("Timestamp,Baro-Chamber PSI,Control Chamber PSI,PWM(%),Calculated PSI");
        myFile.close();
        Serial.println("SD card reinitialized and log file recreated.");
    } else {
        Serial.println("Error: Could not recreate log file.");
    }
}


void updateRamping() {
    if (isRampingToTarget) {
        unsigned long elapsedTime = millis() - rampStartTime;
        float timeInMinutes = elapsedTime / 60000.0;

        // Calculate currentPSI toward targetPSI
        currentPSI = rampStartPSI + (timeInMinutes * rampToTargetIncrement);

        // Debugging: Check targetPSI remains unchanged and correct
        Serial.print("Ramping to target PSI... CurrentPSI: ");
        Serial.print(currentPSI);
        Serial.print(" | TargetPSI: ");
        Serial.print(targetPSI);
        Serial.print(" | RampStartPSI: ");
        Serial.println(rampStartPSI);

        // Check if the target PSI is reached with tolerance
        if (currentPSI >= targetPSI - 0.1) {
            currentPSI = targetPSI;
            isRampingToTarget = false;  // Mark as complete
            Serial.println("Target PSI reached. Ramp-to-target completed.");
        }

        // Update the PWM based on the currentPSI to maintain ramp
        currentPWM = calculatePWM(currentPSI);
        analogWrite(pwmPin, (int)(currentPWM / 100.0 * 255));
    }
    // Other ramping conditions for regular ramp if applicable
}



// Function to get current time from NTP server as a formatted string
String getNTPTime() {
    memset(packetBuffer, 0, ntpPacketSize);
    ntpUDP.beginPacket(ntpServer, 123);  // Send request to NTP server on port 123
    packetBuffer[0] = 0b11100011;        // Request packet settings
    ntpUDP.write(packetBuffer, ntpPacketSize);
    ntpUDP.endPacket();

    for (int retries = 0; retries < 5; retries++) {
        delay(1000);
        if (ntpUDP.parsePacket()) {
            ntpUDP.read(packetBuffer, ntpPacketSize);
            unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
            unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
            unsigned long secsSince1900 = (highWord << 16 | lowWord);

            // Convert NTP time to Unix time (subtract seventy years in seconds)
            unsigned long epoch = secsSince1900 - seventyYears;

            // Adjust for PDT timezone (UTC-7)
            epoch += 3600 * -7;

            // Calculate time components
            unsigned long days = epoch / 86400L;
            unsigned long secondsInDay = epoch % 86400L;
            int hour = secondsInDay / 3600;
            int minute = (secondsInDay % 3600) / 60;
            int second = secondsInDay % 60;

            // Calculate year, month, and day
            int year = 1970;
            while (days >= 365) {
                if (isLeapYear(year)) {
                    if (days >= 366) {
                        days -= 366;
                        year++;
                    }
                } else {
                    days -= 365;
                    year++;
                }
            }

            int month = 1;
            int daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            if (isLeapYear(year)) daysInMonth[1] = 29;
            while (days >= daysInMonth[month - 1]) {
                days -= daysInMonth[month - 1];
                month++;
            }
            int day = days + 1;

            // Format the timestamp as "YYYY-MM-DD HH:MM:SS"
            char timestamp[20];
            sprintf(timestamp, "%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
            return String(timestamp);
        }
    }
    return "N/A";  // Return "N/A" if retries fail
}

bool isLeapYear(int year) {
    return ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
}



float readBaroChamberPSI() {
    int analogValue = analogRead(baroChamberPin);
    float voltage = (analogValue / 1023.0) * 5.0;
    return voltage >= 1.74 ? 22.9 : (voltage / 1.74) * 22.9;
}

float readControlChamberPSI() {
    int analogValue = analogRead(controlChamberPin);
    float voltage = (analogValue / 1023.0) * 5.0;
    if (voltage <= 1.462) return 0.0;
    if (voltage >= 5.0) return 15.0;
    return (voltage - 1.462) * (15.0 / (5.0 - 1.462));
}

float calculatePSI(float pwm) {
    if (pwm <= pwmPercentages[0]) return psiValues[0];
    if (pwm >= pwmPercentages[dataSize - 1]) return psiValues[dataSize - 1];
    for (int i = 0; i < dataSize - 1; i++) {
        if (pwm >= pwmPercentages[i] && pwm <= pwmPercentages[i + 1]) {
            float t = (pwm - pwmPercentages[i]) / (pwmPercentages[i + 1] - pwmPercentages[i]);
            return psiValues[i] + t * (psiValues[i + 1] - psiValues[i]);
        }
    }
    return 0;
}

float calculatePWM(float psi) {
    if (psi <= psiValues[0]) return pwmPercentages[0];
    if (psi >= psiValues[dataSize - 1]) return pwmPercentages[dataSize - 1];
    for (int i = 0; i < dataSize - 1; i++) {
        if (psi >= psiValues[i] && psi <= psiValues[i + 1]) {
            float t = (psi - psiValues[i]) / (psiValues[i + 1] - psiValues[i]);
            return pwmPercentages[i] + t * (pwmPercentages[i + 1] - pwmPercentages[i]);
        }
    }
    return 0;
}

// Send the web page with the current PWM, PSI state, and sensor inputs
void sendWebPage(WiFiClient& client) {
    client.println("HTTP/1.1 200 OK");
    client.println("Content-type:text/html");
    client.println();

    float baroChamberPSI = readBaroChamberPSI();  
    float controlChamberPSI = readControlChamberPSI();  

    client.println("<html><body>");
    client.println("<h1>Baro-Chamber Pressure Control</h1>");

    // Display the current PWM and PSI values without resetting
    client.println("<h2>Current PWM Output: " + String(currentPWM) + "%</h2>");
    client.println("<h2>Current Calculated Pressure: " + String(currentPSI) + " PSI</h2>");
    client.println("<h2>Baro-chamber Actual Pressure: " + String(baroChamberPSI) + " PSI</h2>");
    client.println("<h2>Control Chamber Actual Pressure: " + String(controlChamberPSI) + " PSI</h2>");

    client.println("<h2>Data Logging: " + String(dataLoggingEnabled ? "Enabled" : "Disabled") + "</h2>");
    client.println("<h2><a href=\"/toggleLogging\">Toggle Data Logging</a></h2>");

    // Add the link to reinitialize the SD card
    client.println("<h2><a href=\"/reinitializeSD\">Reinitialize SD Card</a></h2>");

    // Manual Control
    String manualColor = manualControlActive ? "green" : "red";
    client.println("<h2 style=\"color:" + manualColor + ";\">Manual Control:</h2>");
    client.println("<div style='display: flex; justify-content: space-between;'>");
    for (float p = 0; p <= 100; p += 2.5) {
        float psi = calculatePSI(p);
        client.print("<div><a href=\"/setPWM/");
        client.print(p);
        client.print("\">Set ");
        client.print(p);
        client.println("%</a><br>");
        client.println("Calculated Pressure: " + String(psi) + " PSI</div>");
    }
    client.println("</div>");

    // Ramp from setpoints
    String rampColor = ramping ? "green" : "red";
    client.println("<h2 style=\"color:" + rampColor + ";\">Ramp from Setpoints:</h2>");
    client.println("<div style='display: flex; justify-content: space-between;'>");
    for (float startP = 0; startP <= 77.5; startP += 2.5) {
        float psi = calculatePSI(startP);
        client.print("<div><a href=\"/startRamp/");
        client.print(startP);
        client.print("\">Ramp from ");
        client.print(startP);
        client.println("%</a><br>");
        client.println("Calculated Pressure: " + String(psi) + " PSI</div>");
    }
    client.println("</div>");

    // Ramp to target pressure
    String rampToTargetColor = rampToTarget ? "green" : "red";
    client.println("<h2 style=\"color:" + rampToTargetColor + ";\">Ramp to Target Pressure:</h2>");
    client.println("<div style='display: flex; justify-content: space-between;'>");
    for (float targetP = 8.0; targetP <= 17.0; targetP += 0.5) {
        client.print("<div><a href=\"/rampToTarget/");
        client.print(targetP);
        client.print("\">Ramp to ");
        client.print(targetP);
        client.println(" PSI</a></div>");
    }

    client.println("</body></html>");
}
