#pragma once
#include <Arduino.h>

struct DayForecast {
  time_t date;
  int code;
  float low, high;
};

struct HourForecast {
  int hour;      // local hour 0..23
  int code;
  float temp;
  bool isDay;
};

struct Weather {
  bool valid = false;
  uint32_t fetchedAt = 0;   // millis()
  uint32_t version = 0;     // bumps whenever the forecast or its error changes
  float temp = 0, feelsLike = 0, wind = 0;
  int humidity = 0, code = 0;
  bool isDay = true;
  char sunrise[6] = "", sunset[6] = "";
  DayForecast days[5];
  int numDays = 0;
  HourForecast hours[6];   // this hour and the next five
  int numHours = 0;
  int windDir = 0;         // degrees, direction the wind blows from
  char error[64] = "";
  char city[40] = "";      // place this forecast is for
};

// Where the forecast is for. Chosen in Settings, kept in NVS. Default: Cairo, Egypt.
struct WeatherLocation {
  char name[40] = "";        // city; empty until chosen after the country changed
  char admin1[40] = "";
  char country[40] = "";
  char countryCode[3] = "";
  float lat = 0, lon = 0;
  bool hasCity() const { return name[0] != 0; }
};
WeatherLocation weatherLocation();
void weatherSetCountry(const char *code, const char *name);   // clears the city
void weatherSetCity(const char *name, const char *admin1, float lat, float lon);  // refreshes now

// City search (Open-Meteo geocoding), run on its own task so the screen never waits.
struct CityResult {
  char name[40], admin1[40];
  float lat, lon;
};
enum SearchState { SEARCH_IDLE, SEARCH_LOADING, SEARCH_DONE, SEARCH_FAILED };
struct CitySearch {
  SearchState state = SEARCH_IDLE;
  uint32_t id = 0;
  int count = 0;
  CityResult results[20];
  char error[64] = "";
};
uint32_t weatherSearchStart(const char *query, const char *countryCode);  // returns the search id
void weatherSearchGet(CitySearch &out);


void weatherStart();
void weatherStop();
void weatherRefreshNow();
Weather weatherGet();
// WMO weather code to a short description and an ICON_* glyph.
const char *weatherText(int code);
const char *weatherIcon(int code, bool isDay);
