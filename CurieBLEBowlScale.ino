/*
 * Dog Water or Food Bowl Weight Scale for Arduino/Genuino 101.
 * 
 * This Sketch periodically reports, through BLE, the total weight on the scale,
 * for example: the waterproof placement, the water bowl, and the water in the bowl.
 * This data provides enough information for a cloud service to calculate
 * when (and perhaps how much) the dog is drinking and when the bowl is refilled.
 * 
 * Parts Required.  See BillOfMaterials.ods for the full list.
 * - 1 Load Cell
 * - 1 Load Cell Amplifier board (HX711)
 * - 1 Arduino 101 (for sending measurements via BLE)
 * - A Rasperry Pi or such BLE-to-WiFi gateway that uploads the data to the cloud.
 * 
 * BLE (Bluetooth Low Energy) interface:
 * - The scale appears as a standard BLE Weight Scale device.
 * - It reports that it supports only one user
 * - It reports the weight of a single user, in kg.
 * 
 * To Use:
 * - Calibrate:
 *   - Run this Sketch with CALIBRATE_SCALE uncommented.
 *   - Let the scale warm up (1/2 hour or so).
 *   - Take everything off the scale.
 *   - Ideally, leave the room for 10 minutes to gather data that isn't
 *     affected by the vibration of the floor.
 *   - Record the value for zero-load.  This is the Offset value for the HX711.
 *   - Measure the weight of a known weight, such as an exercise weight.
 *     I used a food scale to measure the weight of a nominally 5 pound exercise weight
 *     in kilograms.
 *   - Place the known weight on the scale.
 *   - wait 20 minutes or so to accomodate Load Cell Creep.
 *   - Leave the room for say 10 minutes to gather data without floor vibration.
 *   - Record the value for the known weight.
 *     estimate the Scale value for the Load Cell:
 *       Scale = (known_weight_output - empty_scale_output) / known_weight_in_kg
 *     Don't worry if the readings are negative; the math works for negative numbers as well.
 *     
 * - Run:
 *   - Comment out CALIBRATE_SCALE.
 *   - Set CELL_OFFSET and CELL_SCALE to the values you calculated above.
 *   - Start the Sketch.
 *   - Read the reported weights, etc. using a BLE device.
 *   - Typically you'll want to use a BLE-to-WiFi gateway
 *     (such as a Raspberry Pi 3) to send the data to a cloud
 *     data warehouse such as http://data.sparkfun.com
 *     
 * For debugging the BLE communication, I like the Nordic BLE phone apps:
 * https://play.google.com/store/apps/details?id=no.nordicsemi.android.mcp
 * https://play.google.com/store/apps/details?id=no.nordicsemi.android.nrftoolbox
 *     
 * Copyright (c) 2016 Bradford Needham, North Plains, Oregon, USA
 * @bneedhamia, https://www.needhamia.com
 * Licensed under the GPL 2, a copy of which
 * should have been included with this software.
 */

#include "HX711.h"     // https://github.com/bogde/HX711 Load Cell Amp library
#include <CurieBLE.h>  // Arduino 101 BLE library.
// 80 column marker follows:
//345678901234567892123456789312345678941234567895123456789612345678971234567898

/*
 * Advertised name of our BLE device (the scale).
 * You can change this to whatever you want,
 * as long as the gateway knows the name.
 */
const char *BLE_LOCAL_NAME = "K9 Water";

/*
 * CALIBRATE_SCALE = uncomment this line to report the raw
 *  Load Cell Amplifier value for calculating calibration.
 * Comment out this line to run the normal scale software.
 */
//#define CALIBRATE_SCALE 1

/*
 * Calibrated values of the linear equation for
 * this specific Load Cell Amplifier + Load Cell.
 * You will need to recalibrate if you replace the Load Cell
 *   or the Load Cell Amplifier board.
 *   
 * CELL_OFFSET = zero-load offset for the Load Cell,
 *   calculated from CALIBRATE_SCALE readings with nothing on the scale.
 * CELL_SCALE_KG = scale from Amplifier units to kg for that Load Cell.
 *   That is, number of Amplifier reading units per Kilogram.
 *   Calculated from CALIBRATE_SCALE readings described above.
 */

const long CELL_OFFSET = -22050L;
const float CELL_SCALE_KG = -219327.839;

/*
 * MS_PER_WEIGHT_READING = time (ms) for each reading the scale weight.
 *   That is, the time per record of data that could be uploaded to the cloud.
 *   The shorter this time is, the more data can be uploaded.
 *   The longer this time is, the more likely we'll miss some drinking.
 */
const unsigned long MS_PER_WEIGHT_READING = 10 * 1000L;

/*
 * SAMPLES_PER_WEIGHT = number of raw values to average
 *   per reading from the Load Cell.
 *   The higher this number, the less noise will appear in readings.
 *   The lower this number, the quicker the readings will occur,
 *   which will make the scale more accurate when the dog is drinking/eating.
 */
const int SAMPLES_PER_WEIGHT = 5;

/*
 * I/O pins:
 *
 * PIN_HX711_* = CLock and Data pins for the Load Cell Amplifier.
 */
const int PIN_HX711_CLK = 2;
const int PIN_HX711_DOUT = 3;

#ifdef CALIBRATE_SCALE
/*
 * SAMPLES_PER_READING = number of samples per calibration reading.
 * values[] = individual readings to do statistics on.
 */
const int SAMPLES_PER_READING = 40;
float values[SAMPLES_PER_READING];
#endif

/*
 * hx711 = controller for the HX711 Load Cell Amplifier.
 */
HX711 hx711(PIN_HX711_DOUT, PIN_HX711_CLK);

/*
 * The BLE Objects we use to send data.
 * We send a Standard BLE "Weight Scale" Service,
 *   just like a BLE bathroom scale.
 * See https://developer.bluetooth.org/gatt/services/Pages/ServicesHome.aspx
 * for constants and encoding of BLE data for various purposes.
 * 
 * ble = root of our BLE Peripheral (Server; the Arduino)
 * bleWeightService = BLE service to transmit the total weight on the scale.
 * bleWeightFeature = the features supported by this weight device.
 *   The '4' = 4 bytes are required to store the (32-bit) value.
 * bleWeightMeasurement = the weight measurement itself.
 *   The '3' = 3 bytes are required to store the value:
 *   1 byte of flags and 2 bytes of weight.
 */

BLEPeripheral ble;
BLEService bleWeightService("181D"); 
BLECharacteristic bleWeightFeature("2A9E", BLERead | BLENotify, 4);
BLECharacteristic bleWeightMeasurement("2A9D", BLERead | BLENotify, 3);

/*
 * weight_kg = Weight (kg) read from the Load Cell.
 * This weight includes everything on the scale:
 * the bowl plus the food or water in it.
 */
float weight_kg;

/*
 * previousWeightTimeMs = Time (ms since reset) of the
 *   most recent weight reading.
 * Used to control how often we measure new Load Sensor values.
 */
unsigned long previousWeightTimeMs;


void setup() {
  Serial.begin(9600);

#ifdef CALIBRATE_SCALE
  // On Arduino 101, wait for the PC to open the port.
  while (!Serial);
  
  Serial.print(F("Calibrating, "));
  Serial.print(SAMPLES_PER_READING);
  Serial.println(F(" Samples per row."));

  // This line is formatted so you can import into a spreadsheet
  //TODO alternatively, think about reporting the cal numbers
  // via a custom BLE value, so you can write a better calibration app.
  Serial.println(F("Average,Std Dev"));

#else
     
  /*
   * Load the calibrations into the Amplifier object.
   * For the linear equation Y = MX + B,
   *   where X = weight and Y = Load Cell Value,
   *   set_scale() sets M,
   *   set_offset() sets B.
   */

  hx711.set_scale(CELL_SCALE_KG);
  hx711.set_offset(CELL_OFFSET);

  /*
   * Set the previous reporting time so that
   *   we will wait one interval before our second reading.
   */
  previousWeightTimeMs = millis() + MS_PER_WEIGHT_READING;

  // Setup BLE (Bluetooth Low Energy)
  ble.setLocalName(BLE_LOCAL_NAME);
  ble.setAdvertisedServiceUuid(bleWeightService.uuid());
  ble.addAttribute(bleWeightService);
  ble.addAttribute(bleWeightFeature);
  ble.addAttribute(bleWeightMeasurement);

  // Make our first measurement so we have something to report.
  weight_kg = hx711.get_units(SAMPLES_PER_WEIGHT);
  Serial.println(weight_kg, 3);
  
  /*
   * Initialize our BLE Characteristics from that measurement
   * so that they have a value when we begin.
   */
  setBleWeightFeature();
  setBleWeightMeasurement(weight_kg);

  // Start the BLE radio
  ble.begin();

#endif

}


void loop() {

#ifdef CALIBRATE_SCALE

  delay(2000);

  /*
   * Read and report the average and Standard Deviation for the Load Cell.
   * Formatted to paste the output into a spreadsheet.
   */
    
  reportStatistics(&hx711);
  Serial.println();
  
#else

  // If it's time to process a sample, do it.
  
  unsigned long now = millis();
  if (now - previousWeightTimeMs > MS_PER_WEIGHT_READING) {
    previousWeightTimeMs = now;

    // read the weight from the load sensor.
    weight_kg = hx711.get_units(SAMPLES_PER_WEIGHT);
    Serial.println(weight_kg, 3);

    // Send the new reading
    setBleWeightMeasurement(weight_kg);
  }
  
#endif
}


/*
 * Initializes our BLE Characteristic
 * that describes what features our Weight device provides.
 */
void setBleWeightFeature() {
  unsigned long val = 0;          // field value
  unsigned char bytes[4] = {0};   // field value, encoded for transmission.
  
  /*
   * Flags.
   * 
   * bit 0 = 0 means no Time stamp provided.
   * bit 1 = 0 means multiple scale users are NOT supported.
   * bit 2 = 0 means no BMI supported.
   * bits 3..6 = 7 means 0.01 kg resolution (that says nothing about accuracy).
   * bits 7..9 = 0 means height resolution unspecified.
   * bits 10..31 are reserved and should be set to 0.
   */

  val |= 0x0 << 0;
  val |= 0x0 << 1;
  val |= 0x0 << 2;
  val |= 0x7 << 3;
  val |= 0x0 << 7;

  // BLE GATT multi-byte values are encoded Least-Significant Byte first.
  bytes[0] = (unsigned char) val;
  bytes[1] = (unsigned char) (val >> 8);
  bytes[2] = (unsigned char) (val >> 16);
  bytes[3] = (unsigned char) (val >> 24);

  bleWeightFeature.setValue(bytes, sizeof(bytes));
}


/*
 * Sets our BLE Characteristic given a weight measurement (in kg)
 * 
 * See https://developer.bluetooth.org/gatt/characteristics/Pages/CharacteristicViewer.aspx?u=org.bluetooth.characteristic.weight_measurement.xml
 *  for details of the encoding of weight in BLE.
 */
void setBleWeightMeasurement(float weightKg) {
  unsigned char flags = 0;      // description of the weight
  uint16_t newVal = 0;          // field value: the weight in BLE format
  unsigned char bytes[3] = {0}; // data, encoded for transmission.
  
  /*
   * Set the flags:
   * bit 0 = 0 means we're reporting in SI units (kg and meters)
   * bit 1 = 0 means there is no time stamp in our report
   * bit 2 = 0 means User ID is NOT in our report
   * bit 3 = 0 means no BMI and Height are in our report
   * bits 4..7 are reserved, and set to zero.
   */

  flags |= 0x0 << 0;
  flags |= 0x0 << 1;
  flags |= 0x0 << 2;
  flags |= 0x0 << 3;

  // Convert the weight into BLE representation
  newVal = (uint16_t) ((weightKg * 1000.0 / 5.0) + 0.5);

  /*
   * Because we are a continuous, periodic measurement device,
   * we set the BLE value and notify any BLE Client every time
   * we make a measurement, even if the value hasn't changed.
   * 
   * If instead we were designed to keep a BLE Client informed
   * of the changing value, we'd set the value only if the value changes.
   */

  bytes[0] = flags;

  // BLE GATT multi-byte values are encoded Least-Significant Byte first.
  bytes[1] = (unsigned char) newVal;
  bytes[2] = (unsigned char) (newVal >> 8);

  bleWeightMeasurement.setValue(bytes, sizeof(bytes));
}

#ifdef CALIBRATE_SCALE
/*
 * Reports calibration statistics for the given Load Cell Amplifier.
 * pHx = points to the corresponding Load Cell Amplifier controller.
 */
void reportStatistics(HX711 *pHx) {
  float average;        // simple mean of values[]
  float stddev;         // standard deviation from the mean.

  // Collect a set of readings.
  for (int i = 0; i < SAMPLES_PER_READING; ++i) {
    values[i] = pHx->read();
  }

  // Calculate the Average (Mean)
  average = 0.0f;
  for (int i = 0; i < SAMPLES_PER_READING; ++i) {
    average += values[i];
  }
  average /= (float) SAMPLES_PER_READING;

  /*
   * Calculate the standard deviation:
   * the square root of the average of the squares
   * of the differences from the Mean.
   */

  stddev = 0;
  for (int i = 0; i < SAMPLES_PER_READING; ++i) {
    stddev += (values[i] - average) * (values[i] - average);
  }
  stddev /= (float) SAMPLES_PER_READING;
  stddev = (float) sqrt(stddev);

  // Output the results, in a form to import into a spreadsheet.
  Serial.print(average, 1);
  Serial.print(F(","));
  Serial.print(stddev, 1);
}
#endif

