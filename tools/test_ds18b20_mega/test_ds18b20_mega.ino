// ============================================================
// Test DS18B20 para Arduino Mega  ---  SIN librerias
// Detecta VARIOS sensores (busqueda 1-Wire + CRC + temperatura)
// ------------------------------------------------------------
// Cableado:
//   - Cable DATOS (amarillo)  -> pin D52
//   - Cable VDD   (rojo)      -> 5V
//   - Cable GND   (negro)     -> GND
//   - Resistencia 4.7k entre D52 y 5V   (imprescindible)
//
// Monitor Serie a 9600. Lista cada sensor con su ROM, si el CRC
// es valido (sensor autentico/buena senal) y su temperatura.
// ============================================================

#define OW 52          // pin de datos 1-Wire (D52 del Mega)
#define MAXDEV 6

// ---- primitivas bit-bang 1-Wire ----
bool owReset() {
  pinMode(OW, OUTPUT); digitalWrite(OW, LOW);
  delayMicroseconds(480);
  pinMode(OW, INPUT);
  delayMicroseconds(70);
  bool present = (digitalRead(OW) == LOW);
  delayMicroseconds(410);
  return present;
}
void owWriteBit(uint8_t b) {
  if (b) { pinMode(OW, OUTPUT); digitalWrite(OW, LOW); delayMicroseconds(6);
           pinMode(OW, INPUT);  delayMicroseconds(64); }
  else   { pinMode(OW, OUTPUT); digitalWrite(OW, LOW); delayMicroseconds(60);
           pinMode(OW, INPUT);  delayMicroseconds(10); }
}
uint8_t owReadBit() {
  pinMode(OW, OUTPUT); digitalWrite(OW, LOW); delayMicroseconds(3);
  pinMode(OW, INPUT); delayMicroseconds(10);
  uint8_t b = digitalRead(OW);
  delayMicroseconds(50);
  return b;
}
void owWrite(uint8_t v) { for (uint8_t i = 0; i < 8; i++) { owWriteBit(v & 1); v >>= 1; } }
uint8_t owRead() { uint8_t r = 0; for (uint8_t i = 0; i < 8; i++) if (owReadBit()) r |= (1 << i); return r; }

// ---- CRC8 Dallas ----
uint8_t crc8;
void docrc8(uint8_t value) {
  crc8 ^= value;
  for (uint8_t i = 0; i < 8; i++)
    crc8 = (crc8 & 1) ? (crc8 >> 1) ^ 0x8C : (crc8 >> 1);
}

// ---- busqueda ROM (Maxim APPNOTE 187) ----
uint8_t ROM_NO[8];
int  LastDiscrepancy, LastFamilyDiscrepancy;
bool LastDeviceFlag;

bool OWSearch() {
  uint8_t id_bit_number = 1, last_zero = 0, rom_byte_number = 0, rom_byte_mask = 1;
  uint8_t id_bit, cmp_id_bit, search_direction;
  bool search_result = false;
  crc8 = 0;

  if (!LastDeviceFlag) {
    if (!owReset()) { LastDiscrepancy = 0; LastDeviceFlag = false; LastFamilyDiscrepancy = 0; return false; }
    owWrite(0xF0);                       // SEARCH ROM
    do {
      id_bit = owReadBit();
      cmp_id_bit = owReadBit();
      if (id_bit && cmp_id_bit) break;   // sin dispositivos en este bit
      if (id_bit != cmp_id_bit) {
        search_direction = id_bit;
      } else {
        if (id_bit_number < LastDiscrepancy)
          search_direction = ((ROM_NO[rom_byte_number] & rom_byte_mask) > 0);
        else
          search_direction = (id_bit_number == LastDiscrepancy);
        if (search_direction == 0) { last_zero = id_bit_number; if (last_zero < 9) LastFamilyDiscrepancy = last_zero; }
      }
      if (search_direction == 1) ROM_NO[rom_byte_number] |= rom_byte_mask;
      else                       ROM_NO[rom_byte_number] &= ~rom_byte_mask;
      owWriteBit(search_direction);
      id_bit_number++; rom_byte_mask <<= 1;
      if (rom_byte_mask == 0) { docrc8(ROM_NO[rom_byte_number]); rom_byte_number++; rom_byte_mask = 1; }
    } while (rom_byte_number < 8);

    if (id_bit_number >= 65) {
      LastDiscrepancy = last_zero;
      if (LastDiscrepancy == 0) LastDeviceFlag = true;
      search_result = true;
    }
  }
  if (!search_result || ROM_NO[0] == 0) { LastDiscrepancy = 0; LastDeviceFlag = false; LastFamilyDiscrepancy = 0; search_result = false; }
  return search_result;
}

void setup() {
  Serial.begin(9600);
  delay(500);
  Serial.println(F("== Test DS18B20 multi-sensor (sin librerias) =="));
  Serial.println(F("Datos en D52, pullup 4.7k a 5V"));
}

void loop() {
  uint8_t roms[MAXDEV][8];
  int count = 0;
  LastDiscrepancy = 0; LastDeviceFlag = false; LastFamilyDiscrepancy = 0;
  while (OWSearch() && count < MAXDEV) { for (int i = 0; i < 8; i++) roms[count][i] = ROM_NO[i]; count++; }

  if (count == 0) {
    pinMode(OW, INPUT); delayMicroseconds(5);
    int idle = digitalRead(OW);
    Serial.print(F("NADA: ningun sensor.  Linea reposo = "));
    Serial.println(idle ? F("ALTA (pullup OK -> falta sensor o datos mal conectados)")
                        : F("BAJA (FALTA pullup 4.7k a 5V, o corto a GND)"));
    delay(1500); return;
  }

  Serial.print(F(">>> Sensores encontrados: ")); Serial.println(count);
  for (int s = 0; s < count; s++) {
    Serial.print(F("  [")); Serial.print(s); Serial.print(F("] ROM:"));
    for (int i = 0; i < 8; i++) { Serial.print(' '); if (roms[s][i] < 16) Serial.print('0'); Serial.print(roms[s][i], HEX); }
    crc8 = 0; for (int i = 0; i < 8; i++) docrc8(roms[s][i]);
    Serial.print(roms[s][0] == 0x28 ? F("  fam 0x28 DS18B20") : F("  fam ??"));
    Serial.println(crc8 == 0 ? F("  CRC ok") : F("  CRC MAL"));
  }

  // convertir en todos a la vez, luego leer cada uno por su ROM
  owReset(); owWrite(0xCC); owWrite(0x44);   // SKIP ROM + CONVERT T
  delay(800);
  for (int s = 0; s < count; s++) {
    owReset(); owWrite(0x55);                // MATCH ROM
    for (int i = 0; i < 8; i++) owWrite(roms[s][i]);
    owWrite(0xBE);                           // READ SCRATCHPAD
    uint8_t sp[9]; for (int i = 0; i < 9; i++) sp[i] = owRead();
    int16_t raw = (int16_t)((sp[1] << 8) | sp[0]);
    Serial.print(F("  [")); Serial.print(s); Serial.print(F("] Temp: ")); Serial.print(raw / 16.0); Serial.println(F(" C"));
  }
  Serial.println(F("-----"));
  delay(2000);
}
