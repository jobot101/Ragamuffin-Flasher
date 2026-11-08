// presets.cpp: GENERATED. World-rhythm presets from the Tribe of Abraham drum sequencer
// (https://github.com/tribeofabraham/drum-sequencer, MIT License, Copyright (c) 2026 tribeofabraham).
// Levels: 0 off, 1 ghost, 2 medium, 3 accent. Regenerate with tribe_parse + mkpresets.
#include "chimes.h"

static const uint8_t lv_rock_basic[48] = {3,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,3,0,0,0,2,0,2,0,2,0,2,0,2,0,2,0,2,0,2,0};
static const uint8_t lv_afrobeat[80] = {3,0,0,1,0,0,3,0,0,0,1,0,0,0,0,0,0,0,0,0,3,0,0,1,0,0,0,0,3,0,0,0,2,0,1,0,2,0,1,0,2,0,1,0,2,0,1,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,3,0,0,3,0,0,3,0,0,3,0,0,3,0};
static const uint8_t lv_reggae_one_drop[64] = {0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,0,0,2,0,0,0,0,0,0,0,2,0,0,2,0,2,0,2,0,2,0,2,0,2,0,2,0,2};
static const uint8_t lv_bossa_nova[64] = {3,0,0,1,0,0,3,0,0,1,0,0,3,0,0,0,0,0,2,0,0,0,0,2,0,0,2,0,0,0,0,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,0,1,0,2,0,1,0,2,0,1,0,2,0,1,0};
static const uint8_t lv_samba[80] = {3,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,1,0,2,0,1,0,0,0,1,0,2,0,1,0,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,0,0,0,2,0,0,2,0,0,0,0,2,0,0,2,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
static const uint8_t lv_funk[64] = {3,0,0,1,0,0,0,0,3,0,3,0,0,0,0,0,0,0,0,0,3,0,1,0,0,1,0,0,3,0,0,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2};
static const uint8_t lv_hiphop[64] = {3,0,0,0,0,0,0,2,3,0,0,3,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,3,0,0,0,2,0,2,0,2,0,2,0,2,0,2,0,2,0,2,0,0,0,0,0,0,0,0,2,0,0,0,0,0,0,0,2};
static const uint8_t lv_disco[64] = {3,0,0,0,3,0,0,0,3,0,0,0,3,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,3,0,0,0,2,0,2,0,2,0,2,0,2,0,2,0,2,0,2,0,0,0,0,2,0,0,0,2,0,0,0,2,0,0,0,2};
static const uint8_t lv_motown[64] = {3,0,0,0,3,0,0,0,3,0,0,0,3,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,3,0,0,0,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1};
static const uint8_t lv_cumbia[80] = {0,0,0,0,3,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,2,0,2,0,2,0,2,0,2,0,2,0,2,0,2,0,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,0,0,2,0,0,0,2,0,0,0,2,0,0,0,2,0};
static const uint8_t lv_dabke[64] = {3,0,0,0,0,0,3,0,0,0,0,0,0,0,3,0,0,0,0,0,3,0,0,0,0,0,0,0,3,0,0,0,0,0,0,2,0,0,0,0,0,0,0,2,0,0,0,0,2,0,2,0,2,0,2,0,2,0,2,0,2,0,2,0};
static const uint8_t lv_second_line[64] = {3,0,0,0,0,0,3,0,0,3,0,0,0,0,0,0,0,0,0,1,3,0,0,1,0,0,2,0,3,0,0,1,2,0,2,0,2,0,2,0,2,0,2,0,2,0,2,0,0,2,0,0,0,2,0,0,0,2,0,0,0,2,0,0};
static const uint8_t lv_highlife[80] = {3,0,0,0,0,0,3,0,0,0,0,0,3,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,3,0,0,0,2,0,2,0,2,0,2,0,2,0,2,0,2,0,2,0,0,0,3,0,0,0,0,3,0,0,3,0,0,0,0,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
static const uint8_t lv_waltz[36] = {3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,0,0,0,2,0,0,0,2,0,1,0,2,0,1,0,2,0,1,0};
static const uint8_t lv_landler[48] = {3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,1,0,0,1,0,0,0,1,0,0,0,1,0,1,0,2,0,1,0,2,0,1,0,2,0};
static const uint8_t lv_west_african_68[60] = {3,0,2,0,2,0,3,0,0,2,0,2,3,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,2,1,1,2,1,1,2,1,1,2,1,1,0,0,0,0,2,0,0,0,0,0,0,2};
static const uint8_t lv_afro_cuban_68[60] = {3,0,2,0,2,3,0,2,0,2,0,0,3,0,0,0,0,0,3,0,0,0,0,0,0,0,0,3,0,0,0,0,0,3,0,0,2,0,1,0,1,0,2,0,1,0,1,0,1,1,1,1,1,1,1,1,1,1,1,1};
static const uint8_t lv_shuffle_blues[72] = {3,0,0,0,0,0,3,0,0,0,0,0,3,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,2,0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0,1};
static const uint8_t lv_take_five[80] = {3,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,3,0,2,0,1,0,2,0,1,0,2,0,1,0,2,0,1,0,2,0,1,0,0,0,0,0,0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0};
static const uint8_t lv_persian_5[80] = {3,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,2,0,0,0,1,0,0,0,2,0,0,0,0,0,2,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0};
static const uint8_t lv_balkan_78[56] = {3,0,3,0,0,0,0,3,0,3,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,3,0,0,2,1,2,1,2,1,1,2,1,2,1,2,1,1,0,0,0,0,0,0,2,0,0,0,0,0,0,2};
static const uint8_t lv_turkish_78[56] = {3,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0,3,0,3,0,0,0,0,3,0,3,0,2,0,1,2,1,2,1,2,0,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
static const uint8_t lv_turkish_98[72] = {3,0,0,3,0,0,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,3,0,0,0,2,0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0,0,2,0,0,2,0,0,0,0,2,0,0,0,2,0,0};
static const uint8_t lv_country[64] = {3,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,3,0,0,0,3,0,2,0,3,0,2,0,3,0,2,0,3,0,2,0,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1};
static const uint8_t lv_country_train[48] = {3,0,0,0,0,0,3,0,3,0,0,0,0,0,3,0,0,0,0,0,3,0,0,0,0,0,0,0,3,0,0,0,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1};
static const uint8_t lv_country_shuffle[72] = {3,0,0,0,0,0,3,0,0,0,0,0,3,0,0,0,0,0,3,0,0,0,0,0,0,0,0,3,0,0,0,0,0,3,0,0,0,0,0,3,0,0,0,0,0,3,0,0,2,0,2,2,0,2,2,0,2,2,0,2,2,0,2,2,0,2,2,0,2,2,0,2};
static const uint8_t lv_bluegrass[64] = {3,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,0,2,0,2,0,2,0,2,0,2,0,2,0,2,0};
static const uint8_t lv_bluegrass_roll[64] = {3,0,0,0,0,0,1,0,3,0,0,0,0,0,1,0,0,0,0,0,3,0,0,0,0,0,0,0,3,0,0,0,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,0,0,0,0,0,0,0,2,0,0,0,0,0,0,0,2};
static const uint8_t lv_bluegrass_waltz[36] = {3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,3,0,2,0,2,0,2,0,2,0,2,0,2,0};

const DrumPreset PRESETS[] = {
  {"Rock Basic", "Standard rock beat - the backbone of Western popular music since the 1950s.", 0, 110, 0, 3, 16, {1,2,3}, lv_rock_basic},
  {"Afrobeat", "Fela Kuti-inspired groove from Lagos, Nigeria. Interlocking patterns create polyrhythmic tension.", 0, 115, 15, 5, 16, {1,2,3,10,9}, lv_afrobeat},
  {"Reggae One Drop", "The one-drop - heartbeat of reggae from Kingston, Jamaica. Kick and snare land together on beat 3.", 0, 78, 10, 4, 16, {1,2,5,3}, lv_reggae_one_drop},
  {"Bossa Nova", "From Rio de Janeiro - the samba-jazz fusion of Joao Gilberto and Tom Jobim.", 0, 135, 0, 4, 16, {1,5,3,10}, lv_bossa_nova},
  {"Samba", "Brazilian samba - surdo and tamborim patterns from the escolas de samba of Rio's Carnival.", 0, 100, 0, 5, 16, {1,2,3,5,10}, lv_samba},
  {"Funk", "James Brown / Clyde Stubblefield funk pocket. Ghost notes on the snare create the greasy feel.", 0, 104, 20, 4, 16, {1,2,3,4}, lv_funk},
  {"Hip-Hop Boom Bap", "East Coast boom bap - the swing-heavy pocket of 90s Golden Era hip-hop.", 0, 90, 35, 4, 16, {1,2,3,4}, lv_hiphop},
  {"Disco", "Four-on-the-floor - the driving force of 1970s dance floors, from Studio 54 to Paradise Garage.", 0, 120, 0, 4, 16, {1,2,3,4}, lv_disco},
  {"Motown", "Detroit soul - Benny Benjamin's tambourine-driven pocket from Hitsville, U.S.A.", 0, 108, 10, 4, 16, {1,2,3,10}, lv_motown},
  {"Cumbia", "Colombian cumbia - indigenous, African, and Spanish fusion from the Caribbean coast.", 0, 95, 0, 5, 16, {1,2,3,10,9}, lv_cumbia},
  {"Dabke", "Levantine dabke - the stomping folk line dance of Lebanon, Syria, Palestine, and Jordan.", 0, 145, 0, 4, 16, {1,2,7,3}, lv_dabke},
  {"New Orleans Second Line", "New Orleans second line - the joyous, syncopated street beat of brass band parades.", 0, 108, 30, 4, 16, {1,2,3,9}, lv_second_line},
  {"Ghanaian Highlife", "Ghanaian highlife - joyful dance music blending West African and Western idioms.", 0, 115, 15, 5, 16, {1,2,3,9,10}, lv_highlife},
  {"Viennese Waltz", "The elegant three-beat pattern of the Viennese ballroom - timeless since Strauss.", 1, 180, 0, 3, 12, {1,2,3}, lv_waltz},
  {"Landler", "An Austrian folk dance in 3/4 - the rustic ancestor of the waltz.", 1, 140, 0, 4, 12, {1,2,5,3}, lv_landler},
  {"West African 6/8", "The foundation of West African rhythm - a 6/8 bell pattern from Ghana, Nigeria, and Guinea.", 2, 110, 0, 5, 12, {9,1,2,10,7}, lv_west_african_68},
  {"Afro-Cuban 6/8", "Afro-Cuban bembe - the sacred 6/8 feel of Santera ceremonies and rumba.", 2, 105, 0, 5, 12, {9,1,2,3,10}, lv_afro_cuban_68},
  {"Blues Shuffle", "The 12/8 triplet shuffle - bedrock of Delta blues, Chicago electric, and early rock 'n' roll.", 6, 80, 0, 3, 24, {1,2,3}, lv_shuffle_blues},
  {"Take Five", "Inspired by Dave Brubeck's iconic 1959 composition - jazz in 5/4 time, grouped 3+2.", 3, 172, 0, 4, 20, {1,2,3,5}, lv_take_five},
  {"Persian 5/4", "Evoking Persian classical music's dastgah system - tombak drumming in 5/4.", 3, 130, 0, 4, 20, {1,2,5,10}, lv_persian_5},
  {"Balkan 7/8", "Bulgarian rachenitsa - the irregular 7/8 dance rhythm, grouped 2+2+3.", 4, 160, 0, 4, 14, {1,2,3,5}, lv_balkan_78},
  {"Turkish 7/8", "Turkish aksak rhythm - the lopsided groove of Anatolian folk music, grouped 3+2+2.", 4, 150, 0, 4, 14, {1,2,3,10}, lv_turkish_78},
  {"Turkish 9/8 Karsilama", "Karsilama - a Turkish dance in 9/8 grouped 2+2+2+3, heard at celebrations.", 5, 140, 0, 4, 18, {1,2,3,9}, lv_turkish_98},
  {"Country", "Classic country two-step - kick on 1 and 3, snare on 2 and 4, driving hi-hat eighths. Nashville, Tennessee.", 0, 120, 0, 4, 16, {1,2,3,5}, lv_country},
  {"Country Train Beat", "The train beat - a driving sixteenth-note pattern that mimics the sound of a freight train rolling down the tracks. A staple of uptempo country.", 0, 130, 0, 3, 16, {1,2,3}, lv_country_train},
  {"Country Shuffle", "A triplet-feel shuffle heard in honky-tonk and Western swing. The groove that makes a barstool sway.", 6, 110, 0, 3, 24, {1,2,3}, lv_country_shuffle},
  {"Bluegrass", "Boom-chuck - the rhythmic engine of bluegrass. Bass on the downbeat, snare on the upbeat. Fast, dry, and forward-leaning. Appalachia.", 0, 140, 0, 4, 16, {1,2,3,10}, lv_bluegrass},
  {"Bluegrass Roll", "A driving bluegrass feel with sixteenth-note momentum - the kind of groove that sits behind a banjo roll or flatpick run.", 0, 155, 0, 4, 16, {1,2,10,5}, lv_bluegrass_roll},
  {"Bluegrass Waltz", "Three-quarter time bluegrass - bass on 1, backbeats on 2 and 3. The feel behind every mountain waltz and gospel number.", 1, 160, 0, 3, 12, {1,2,10}, lv_bluegrass_waltz},
};
const int NUM_PRESETS = sizeof(PRESETS) / sizeof(PRESETS[0]);
