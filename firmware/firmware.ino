#include "Platypus.h"
#include <adk.h>

// Arduino headers used in Platypus.h
// (informs the IDE to link these libraries)
#include <Servo.h>
#include <Scheduler.h>

// JSON parsing library
#include "jsmn.h"

// TODO: remove me
#include "Board.h"

// ADK USB Host configuration 
char applicationName[] = "Platypus Server"; // the app on Android
char accessoryName[] = "Platypus Control Board"; // your Arduino board
char companyName[] = "Platypus LLC";
char versionNumber[] = "3.0";
char serialNumber[] = "3";
char url[] = "http://senseplatypus.com";

// ADK USB Host
USBHost Usb;
ADK adk(&Usb, companyName, applicationName, accessoryName, versionNumber, url, serialNumber);

// Android send/receive buffers
const size_t INPUT_BUFFER_SIZE = 512;
char input_buffer[INPUT_BUFFER_SIZE+1];

const size_t OUTPUT_BUFFER_SIZE = 576;
char output_buffer[OUTPUT_BUFFER_SIZE+1];

// JSON parsing buffers
const size_t NUM_JSON_TOKENS = 32;
jsmn_parser json_parser;
jsmntok_t json_tokens[NUM_JSON_TOKENS];

// Define the systems on this board
// TODO: move this board.h?
platypus::Led rgb_led;

const size_t NUM_MOTORS = 2;
platypus::Motor *motor[NUM_MOTORS] = { 
  new platypus::VaporPro(0), 
  new platypus::VaporPro(1) 
};

const size_t NUM_SENSORS = 4;
platypus::Sensor *sensor[NUM_SENSORS] = { 
  new platypus::AnalogSensor(0),
  new platypus::ES2(1),
  new platypus::AnalogSensor(2),
  new platypus::AnalogSensor(3)
};

/**
 * Returns a JSON error message to the connected USB device and the
 * serial debugging console.
 */
void reportJsonError(jsmnerr_t error)
{
  char *error_message;
  
  // Fill in the appropriate JSON error description.
  switch(error) {
    case JSMN_SUCCESS:
      // If we were successful, there is nothing to report.
      return;
    case JSMN_ERROR_NOMEM:
      error_message = "Insufficient memory.";
      break;
    case JSMN_ERROR_INVAL:
      error_message = "Invalid JSON string.";
      break;
    case JSMN_ERROR_PART:
      error_message = "Incomplete JSON string.";
      break;
    default:
      error_message = "Unknown JSON error.";
      break;
  }
  
  // Construct a JSON error message.
  snprintf(output_buffer, OUTPUT_BUFFER_SIZE, 
           "{"
           "\"error\": \"%s\","
           "\"args\": \"%s\""
           "}",
           error_message, input_buffer);
  
  // Report error over USB. 
  adk.write(strlen(output_buffer), (uint8_t*)output_buffer);
  
  // TODO: if performance issue, remove this line.
  // Report error to debugging console.
  Serial.println(output_buffer);
}

/**
 * Copies a JSON string token into a provided char* buffer.
 */
jsmnerr_t json_string(const jsmntok_t &token, const char *json_str, char *output_str, size_t output_len)
{
  if (token.type != JSMN_STRING) return JSMN_ERROR_INVAL;
  
  size_t len = min(token.end - token.start, output_len - 1);
  strncpy(output_str, &json_str[token.start], len);
  output_str[len] = '\0';
  
  return JSMN_SUCCESS;
}

void setup() 
{
  // Initialize debugging serial console.
  Serial.begin(115200);
  
  // Make the ADK buffers into null terminated string.
  input_buffer[INPUT_BUFFER_SIZE] = '\0';
  output_buffer[OUTPUT_BUFFER_SIZE] = '\0';
  
  // Initialize the JSON parser.
  jsmn_init(&json_parser);
  
  // Create a task that decays motor velocity.
  // TODO: move this into Motor class.
  Scheduler.startLoop(motorDecayLoop);
}

void loop() 
{
  // Number of bytes received from USB.
  uint32_t bytes_read = 0;
  
  // Result of JSON parsing.
  jsmnerr_t json_result;

  // Attempt to create USB connection.
  Usb.Task();
  
  // Shutdown system if not connected to USB.
  if (!adk.isReady())
  {
    // Set LED to red when USB is not connected.
    rgb_led.set(1,0,0);
    
    // Turn off motors.
    for (size_t motor_idx = 0; motor_idx < NUM_MOTORS; ++motor_idx) {
      motor[motor_idx]->disable();
    }
    
    // Wait for USB connection again.
    return;
  }
  
  // Set LED to green when USB is connected.
  rgb_led.set(0,1,0); 
    
  // Read next command from USB.
  adk.read(&bytes_read, INPUT_BUFFER_SIZE, (uint8_t*)input_buffer);
  if (bytes_read <= 0) return;
  
  // TODO: remove this debug print
  Serial.print("RCV: ");
  Serial.println(input_buffer);
    
  // Parse command as JSON string
  json_result = jsmn_parse(&json_parser, input_buffer, json_tokens, NUM_JSON_TOKENS);
  
  // Check for valid result, report error on failure.
  if (json_result != JSMN_SUCCESS)
  {
    reportJsonError(json_result);
    return;
  }
  
  // Get the first token and make sure it is a JSON object.
  jsmntok_t *token = json_tokens;
  if (token->type != JSMN_OBJECT) 
  {
    reportJsonError(JSMN_ERROR_INVAL);
    return;
  }
  
  // Read each field of the JSON object and act accordingly.
  size_t num_entries = token->size;
  for (size_t entry_idx = 0; entry_idx < num_entries; entry_idx++)
  {
    // Get the name field for this entry.
    token++;
    char entry_name[64];
    if (json_string(*token, input_buffer, entry_name, 64)) {
      reportJsonError(JSMN_ERROR_INVAL);
      return;
    }
    
    // The following token should always be the entry object.
    token++;
    if (token->type != JSMN_OBJECT) {
      reportJsonError(JSMN_ERROR_INVAL);
      return;      
    }

    // Attempt to decode the configurable object for this entry.
    platypus::Configurable *entry_object;
    
    // If it is a motor, it must take the form 'm1'.
    if (!strncmp("m", entry_name, 1))
    {
      size_t motor_idx = entry_name[1] - '0';
      if (motor_idx >= NUM_MOTORS || entry_name[2] != '\0') {
        reportJsonError(JSMN_ERROR_INVAL);
        return;
      }
      entry_object = motor[motor_idx];
    }
    // If it is a motor, it must take the form 's1'.
    if (!strncmp("s", entry_name, 1))
    {
      size_t sensor_idx = entry_name[1] - '0';
      if (sensor_idx >= NUM_SENSORS || entry_name[2] != '\0') {
        reportJsonError(JSMN_ERROR_INVAL);
        return;
      }
      entry_object = sensor[sensor_idx];
    }
    // Report parse error if unable to identify this entry.
    else {
      reportJsonError(JSMN_ERROR_INVAL);
      return;
    }
    
    // Iterate through each parameter.
    size_t num_params = token->size;
    for (size_t param_idx = 0; param_idx < num_params; param_idx++)
    {
      // Get the name field for this parameter.
      token++;
      char param_name[64];
      if (json_string(*token, input_buffer, param_name, 64)) {
        reportJsonError(JSMN_ERROR_INVAL);
        return;
      }
      
      // Get the value field for this parameter.
      token++;
      char param_value[64];
      if (json_string(*token, input_buffer, param_value, 64)) {
        reportJsonError(JSMN_ERROR_INVAL);
        return;
      }
      
      // Pass this parameter to the entry object.
      entry_object->set(param_name, param_value);
    }
  }
}

void motorDecayLoop()
{
  // Wait for a fixed time period.
  delay(100);
  
  // Decay all motors exponentially towards zero speed.
  for (size_t motor_idx = 0; motor_idx < NUM_MOTORS; ++motor_idx) {
    motor[motor_idx]->velocity(motor[motor_idx]->velocity() * 0.9);
  }
  
  // TODO: move this to another location (e.g. Motor)
  // Send motor status update over USB
  char motor_update[64];
  snprintf(motor_update, 64,
    "{"
      "\"m0\":{"
        "\"v\":%f,"
        "\"c\":%f"
      "},"
      "\"m1\":{"
        "\"v\":%f,"
        "\"c\":%f"
      "}"
    "}",
    motor[0]->velocity(), motor[0]->current(),
    motor[1]->velocity(), motor[1]->current()
  );
  adk.write(strlen(motor_update), (uint8_t *)motor_update);
}


