// include the library code:
#include <Wire.h>
#include <Adafruit_RGBLCDShield.h>
#include <utility/Adafruit_MCP23017.h>


// The shield uses the I2C SCL and SDA pins. On classic Arduinos
// this is Analog 4 and 5 so you can't use those for analogRead() anymore
// However, you can connect other I2C sensors to the I2C bus and share
// the I2C bus.
Adafruit_RGBLCDShield lcd = Adafruit_RGBLCDShield();

// These #defines make it easy to set the backlight color
#define RED 0x1
#define YELLOW 0x3
#define GREEN 0x2
#define TEAL 0x6
#define BLUE 0x4
#define VIOLET 0x5
#define WHITE 0x7

#define LCD_COLUMN_COUNT 16
#define LCD_ROW_COUNT 2

#define PIN_IS_NOT_DIALING 32
#define PIN_TICK 38

#define DENOISE_THRESHOLD 3
#define UNSTABLE 42069

#define PHONE_NUMBER_LENGTH_MAX 20
#define MESSAGE_LENGTH_MAX 50

#define NUMBER_COUNT 10  // Cardinality of the numerical input.
#define PIN_COUNT 50  // Number of IO pins (for fluctuation buffering).
#define BUTTON_COUNT 5 // Number of physical IU buttons.
enum input_mode {
  input_mode_dialer,
  input_mode_t9
};

input_mode current_mode = input_mode_dialer;

uint16_t clock_counter = 0;
uint16_t clock_counter_k = 0;

/**
 * State for raw dialer IO - counting clicks and reporting the digit.
 */
// We track whether or not we are in dialing mode -- if so, keep counting.
uint8_t state_is_dialing = 0;
// We count the number of downward edges, so if we transition high to low, we increment.
uint8_t state_is_tick_high = 1;
uint8_t n_ticks_counted = 0;
uint8_t k_clocks_from_last_entry = 0;

uint8_t pin_states[PIN_COUNT];
uint8_t pin_states_readcount[PIN_COUNT];

uint8_t button_state_[BUTTON_COUNT];

uint8_t output_cursor_row = 0;
uint8_t output_cursor_col = 0;
bool t9_auto_advance_on  = false;
bool t9_in_multicharacter_selection = false;

/**
 * State for T9 messaging.
 * 
 */
uint8_t number_dial_counts[NUMBER_COUNT];
// Hello: 4,4; 3,3; 5,5,5; 5,5,5; 6,6,6; 0;
// World 9; 6,6,6; 7,7,7; 5,5,5; 3
static const char t9[][4] = {
  {' ',NULL,NULL,NULL},   // 0
  {NULL,NULL,NULL,NULL},  // 1
  {'a','b','c',NULL},     // 2
  {'d','e','f',NULL},     // 3
  {'g','h','i',NULL},     // 4
  {'j','k','l',NULL},     // 5
  {'m','n','o',NULL},     // 6
  {'p','q','r','s'},      // 7
  {'t','u','v',NULL},     // 8
  {'w','x','y','z'}};     // 9
static const char numbers_lookup[10] = {'0','1','2','3','4','5','6','7','8','9'};
bool is_initial_number = true;

void setup() {
  // Debugging output
  Serial.begin(9600);
  // set up the LCD's number of columns and rows: 
  lcd.begin(16, 2);
  lcd.blink();
  lcd.cursor();
  lcd.setBacklight(GREEN);
  current_mode = input_mode_dialer;
  pinMode(PIN_IS_DIALING, INPUT);
  pinMode(PIN_TICK, INPUT);
  memset(pin_states, 0, PIN_COUNT);
  memset(pin_states_readcount, 0, PIN_COUNT);
  memset(button_state_, 0, BUTTON_COUNT);
  
  pin_states[PIN_TICK] = 1;  // The 'tick' signal starts high.
}

/**
 * Entrypoint for data entry, called when a new new number 
 * is dialed from the rotary interface. This follows the counting
 * of "ticks" from the rotary interface; at this point we are confident
 * that the given number was dialed.
 * This branches into "T9" or "num" interpretation, depending on the
 * mode we are in.
 */
void new_number_dialed(uint8_t new_number) {
  if (current_mode == input_mode_dialer) {
    num_new_number_entered(new_number);
  } else {
    t9_new_number_entered(new_number);
  }
}

/**
 * Number mode: New number entered.
 * This is the trivial case, just print the number and increment
 * the column.
 */
void num_new_number_entered(uint8_t new_number) {
  Serial.print("\nnum_new_number_entered() new_number=");
  Serial.print(numbers_lookup[new_number]);
  lcd.print(numbers_lookup[new_number]);
  output_cursor_col++;
}

/**
 * T9 mode: New number entered.
 * We need to figure out whether the current character is being incremented
 * or whether this is a new character.
 */
void t9_new_number_entered(uint8_t new_number) {
  Serial.print("\nnew_number=");
  Serial.print(new_number);
  uint8_t current_count = number_dial_counts[new_number];
  Serial.print("\ncurrent_count=");
  Serial.print(current_count);

  bool encountered_new_number = (current_count == 0);
  Serial.print("\nencountered_new_number=");
  Serial.print(encountered_new_number);
  
  k_clocks_from_last_entry = 1; // reset auto_advance timer.
  clock_counter = 0;
  Serial.print("\t9[new_number][current_count]=");
  Serial.print(t9[new_number][current_count]);

  // Zero out any previous counters when we...
  if (current_count == 0 ||  // ...receive a new number or...
      current_count >= 4 || !t9[new_number][current_count]) { // ...roll over.
    memset(number_dial_counts, 0, NUMBER_COUNT);
    current_count = 0;
  }
  if (t9[new_number][current_count] == NULL) {
    // For numbers without a character, return after resetting character state.
    t9_in_multicharacter_selection = false;
    return;
  }
  
  if (t9_in_multicharacter_selection && encountered_new_number && !is_initial_number) {
    output_cursor_col++;
    Serial.print("\noutput_cursor_col++ evaluated in t9_new_number_entered!");
  }
  lcd.setCursor(output_cursor_col, output_cursor_row);
  lcd.print(t9[new_number][current_count]);
  is_initial_number = false;
  t9_auto_advance_on = true;
  number_dial_counts[new_number]++;
  t9_in_multicharacter_selection = true;
  Serial.print("\nat end of t9_new_number_entered: number_dial_counts[new_number]=");
  Serial.print(number_dial_counts[new_number]);
}

/**
 * Low level input handling.
 */
void get_buttons() {
  uint8_t buttons = lcd.readButtons();
  if (buttons & BUTTON_UP) {
    output_cursor_row = 0;
    output_cursor_col = 0;
    lcd.setBacklight(GREEN);
    current_mode = input_mode_dialer;
  }
  if (buttons & BUTTON_DOWN) {
    output_cursor_row = 1;
    output_cursor_col = 0;
    lcd.setBacklight(YELLOW);
    current_mode = input_mode_t9;
  }

  if (!button_state_[2] && (buttons & BUTTON_LEFT)) {
    if (output_cursor_col > 0) {
      --output_cursor_col;
    }
  }
  button_state_[2] = (buttons & BUTTON_LEFT);
  
  if (!button_state_[3] && (buttons & BUTTON_RIGHT)) {
    if (output_cursor_col < LCD_COLUMN_COUNT) {
      ++output_cursor_col;
    }
  }
  button_state_[3] = (buttons & BUTTON_RIGHT);
  lcd.setCursor(output_cursor_col, output_cursor_row);
}

uint8_t get_pin_state(uint8_t pin_address) {
  uint8_t current_value = digitalRead(pin_address);
  if (pin_states[pin_address] == current_value) {
    pin_states_readcount[pin_address]++;
  } else {
    pin_states[pin_address] = current_value;
    pin_states_readcount[pin_address] = 1;
  }
  if (pin_states_readcount[pin_address] >= DENOISE_THRESHOLD) {
    return pin_states[pin_address];
  } else {
    return UNSTABLE;
  }
}

void get_number() {
  uint8_t cur_pin_is_dialing = get_pin_state(PIN_IS_DIALING);
  uint8_t cur_pin_tick = get_pin_state(PIN_TICK);

  if (cur_pin_is_dialing == UNSTABLE || cur_pin_tick == UNSTABLE) {
    return;
  }
  // The "is dialing" pin is low unless a number is being dialed.
  if (!state_is_dialing && !digitalRead(PIN_IS_DIALING)) {
    state_is_dialing = 1;
  } else if (state_is_dialing && digitalRead(PIN_IS_DIALING)) {
    // We are done dialing! If we have some number of ticks, let's report them.
    state_is_dialing = 0;
    n_ticks_counted = n_ticks_counted % 10;
    new_number_dialed(n_ticks_counted);
    n_ticks_counted = 0;
  }
  // The "tick" pin is high, and pulled low for every tick.
  if(state_is_tick_high && !digitalRead(PIN_TICK)) {
    state_is_tick_high = 0;
    n_ticks_counted = n_ticks_counted + 1;
    Serial.print("\ndigitalRead(PIN_IS_DIALING)=");
    Serial.print(digitalRead(PIN_IS_DIALING));
    Serial.print("\nCLICK!");
  } else if (!state_is_tick_high && digitalRead(PIN_TICK)) {
    state_is_tick_high = 1;
  }
}

void loop() {
  get_buttons();
  get_number();

  // Auto-advance is enabled when a new T9 input is registered.
  if (t9_auto_advance_on ) {
    ++clock_counter;
    if (clock_counter % 100 == 0) {
      ++k_clocks_from_last_entry;
      Serial.print("\nk_clocks_from_last_entry=");
      Serial.print(k_clocks_from_last_entry);
      clock_counter = 0;
    }
    if (k_clocks_from_last_entry > 5) {
      ++output_cursor_col;
      lcd.setCursor(output_cursor_col, output_cursor_row);
      k_clocks_from_last_entry = 0;
      t9_in_multicharacter_selection = false;
      t9_auto_advance_on = false;  // Don't auto-advance into infinity!
    }
  }
}
