/*
 * OpenChime client core — composer completion and the emoji catalogue.
 * See complete.h.
 */

#include "complete.h"
#include "protocol.h"      /* OC_CHANNEL_KIND_DM */

#include <ctype.h>
#include <string.h>
#include <stdio.h>

/* ---- the catalogue --------------------------------------------------------
 * Ordered by category so a picker can walk it once and emit section headers
 * without sorting. Shortcodes follow the names people already type elsewhere. */
#define S OC_EMOJI_CAT_SMILEYS
#define G OC_EMOJI_CAT_GESTURES
#define P OC_EMOJI_CAT_PEOPLE
#define N OC_EMOJI_CAT_NATURE
#define F OC_EMOJI_CAT_FOOD
#define A OC_EMOJI_CAT_ACTIVITY
#define O OC_EMOJI_CAT_OBJECTS
#define Y OC_EMOJI_CAT_SYMBOLS

/* Every row states its `tonable` flag, including the 767 that are 0.
 * Relying on C's zero rule left the table one field short of the struct, which
 * is exactly what -Wmissing-field-initializers is for: a field appended to
 * oc_emoji later would silently read 0 for every emoji here and nothing would
 * say so. Writing it out costs a column and makes the omission impossible. */
static const oc_emoji EMOJI[] = {
    /* smileys */
    {"smile","\xf0\x9f\x98\x84","happy joy",S,0},
    {"grin","\xf0\x9f\x98\x81","happy",S,0},
    {"laughing","\xf0\x9f\x98\x86","lol haha",S,0},
    {"joy","\xf0\x9f\x98\x82","lol crying laugh",S,0},
    {"rofl","\xf0\x9f\xa4\xa3","lol rolling",S,0},
    {"slightly_smiling_face","\xf0\x9f\x99\x82","smile",S,0},
    {"upside_down_face","\xf0\x9f\x99\x83","silly",S,0},
    {"wink","\xf0\x9f\x98\x89","joke",S,0},
    {"blush","\xf0\x9f\x98\x8a","shy happy",S,0},
    {"innocent","\xf0\x9f\x98\x87","angel halo",S,0},
    {"smiling_face_with_three_hearts","\xf0\x9f\xa5\xb0","love adore",S,0},
    {"heart_eyes","\xf0\x9f\x98\x8d","love",S,0},
    {"star_struck","\xf0\x9f\xa4\xa9","amazed wow",S,0},
    {"kissing_heart","\xf0\x9f\x98\x98","kiss love",S,0},
    {"kissing","\xf0\x9f\x98\x97","kiss",S,0},
    {"kissing_closed_eyes","\xf0\x9f\x98\x9a","kiss",S,0},
    {"kissing_smiling_eyes","\xf0\x9f\x98\x99","kiss",S,0},
    {"smiling_face_with_tear","\xf0\x9f\xa5\xb2","grateful sad happy",S,0},
    {"sweat_smile","\xf0\x9f\x98\x85","relief phew nervous",S,0},
    {"melting_face","\xf0\x9f\xab\xa0","melting embarrassed",S,0},
    {"saluting_face","\xf0\x9f\xab\xa1","salute yes sir",S,0},
    {"yum","\xf0\x9f\x98\x8b","tasty delicious",S,0},
    {"stuck_out_tongue","\xf0\x9f\x98\x9b","tongue",S,0},
    {"stuck_out_tongue_winking_eye","\xf0\x9f\x98\x9c","tongue joke",S,0},
    {"zany_face","\xf0\x9f\xa4\xaa","crazy silly",S,0},
    {"stuck_out_tongue_closed_eyes","\xf0\x9f\x98\x9d","tongue",S,0},
    {"money_mouth_face","\xf0\x9f\xa4\x91","rich dollar",S,0},
    {"hugs","\xf0\x9f\xa4\x97","hug",S,0},
    {"hand_over_mouth","\xf0\x9f\xa4\xad","oops giggle",S,0},
    {"shushing_face","\xf0\x9f\xa4\xab","quiet secret",S,0},
    {"thinking","\xf0\x9f\xa4\x94","hmm consider",S,0},
    {"zipper_mouth_face","\xf0\x9f\xa4\x90","quiet secret",S,0},
    {"raised_eyebrow","\xf0\x9f\xa4\xa8","skeptical doubt",S,0},
    {"neutral_face","\xf0\x9f\x98\x90","meh",S,0},
    {"expressionless","\xf0\x9f\x98\x91","meh",S,0},
    {"no_mouth","\xf0\x9f\x98\xb6","silent",S,0},
    {"face_in_clouds","\xf0\x9f\x98\xb6\xe2\x80\x8d\xf0\x9f\x8c\xab\xef\xb8\x8f","confused foggy",S,0},
    {"smirk","\xf0\x9f\x98\x8f","smug",S,0},
    {"unamused","\xf0\x9f\x98\x92","meh annoyed",S,0},
    {"roll_eyes","\xf0\x9f\x99\x84","whatever",S,0},
    {"grimacing","\xf0\x9f\x98\xac","awkward yikes",S,0},
    {"exhale","\xf0\x9f\x98\xae\xe2\x80\x8d\xf0\x9f\x92\xa8","relief sigh",S,0},
    {"lying_face","\xf0\x9f\xa4\xa5","liar pinocchio",S,0},
    {"relieved","\xf0\x9f\x98\x8c","calm",S,0},
    {"pensive","\xf0\x9f\x98\x94","sad",S,0},
    {"sleepy","\xf0\x9f\x98\xaa","tired",S,0},
    {"drooling_face","\xf0\x9f\xa4\xa4","want",S,0},
    {"sleeping","\xf0\x9f\x98\xb4","zzz tired",S,0},
    {"mask","\xf0\x9f\x98\xb7","sick ill",S,0},
    {"face_with_thermometer","\xf0\x9f\xa4\x92","sick fever",S,0},
    {"face_with_head_bandage","\xf0\x9f\xa4\x95","hurt injured",S,0},
    {"nauseated_face","\xf0\x9f\xa4\xa2","sick gross",S,0},
    {"vomiting_face","\xf0\x9f\xa4\xae","sick gross",S,0},
    {"sneezing_face","\xf0\x9f\xa4\xa7","sick cold",S,0},
    {"hot_face","\xf0\x9f\xa5\xb5","heat sweating",S,0},
    {"cold_face","\xf0\x9f\xa5\xb6","freezing",S,0},
    {"woozy_face","\xf0\x9f\xa5\xb4","dizzy drunk",S,0},
    {"dizzy_face","\xf0\x9f\x98\xb5","stunned",S,0},
    {"exploding_head","\xf0\x9f\xa4\xaf","mind blown",S,0},
    {"cowboy_hat_face","\xf0\x9f\xa4\xa0","yeehaw",S,0},
    {"partying_face","\xf0\x9f\xa5\xb3","celebrate party",S,0},
    {"disguised_face","\xf0\x9f\xa5\xb8","incognito",S,0},
    {"sunglasses","\xf0\x9f\x98\x8e","cool",S,0},
    {"nerd_face","\xf0\x9f\xa4\x93","geek",S,0},
    {"monocle_face","\xf0\x9f\xa7\x90","inspect examine",S,0},
    {"confused","\xf0\x9f\x98\x95","unsure",S,0},
    {"worried","\xf0\x9f\x98\x9f","concerned",S,0},
    {"slightly_frowning_face","\xf0\x9f\x99\x81","sad",S,0},
    {"frowning_face","\xe2\x98\xb9\xef\xb8\x8f","sad",S,0},
    {"open_mouth","\xf0\x9f\x98\xae","surprised",S,0},
    {"hushed","\xf0\x9f\x98\xaf","surprised",S,0},
    {"astonished","\xf0\x9f\x98\xb2","shocked",S,0},
    {"flushed","\xf0\x9f\x98\xb3","embarrassed",S,0},
    {"pleading_face","\xf0\x9f\xa5\xba","please beg",S,0},
    {"frowning","\xf0\x9f\x98\xa6","sad",S,0},
    {"anguished","\xf0\x9f\x98\xa7","shocked",S,0},
    {"fearful","\xf0\x9f\x98\xa8","scared",S,0},
    {"cold_sweat","\xf0\x9f\x98\xb0","anxious nervous",S,0},
    {"disappointed_relieved","\xf0\x9f\x98\xa5","phew sad",S,0},
    {"cry","\xf0\x9f\x98\xa2","sad tear",S,0},
    {"sob","\xf0\x9f\x98\xad","crying bawling",S,0},
    {"scream","\xf0\x9f\x98\xb1","fear shock",S,0},
    {"confounded","\xf0\x9f\x98\x96","frustrated",S,0},
    {"persevere","\xf0\x9f\x98\xa3","struggle",S,0},
    {"disappointed","\xf0\x9f\x98\x9e","sad",S,0},
    {"sweat","\xf0\x9f\x98\x93","nervous",S,0},
    {"weary","\xf0\x9f\x98\xa9","tired exhausted",S,0},
    {"tired_face","\xf0\x9f\x98\xab","exhausted",S,0},
    {"yawning_face","\xf0\x9f\xa5\xb1","bored tired",S,0},
    {"triumph","\xf0\x9f\x98\xa4","proud steam",S,0},
    {"rage","\xf0\x9f\x98\xa1","angry mad",S,0},
    {"angry","\xf0\x9f\x98\xa0","mad",S,0},
    {"cursing_face","\xf0\x9f\xa4\xac","swearing",S,0},
    {"smiling_imp","\xf0\x9f\x98\x88","devil mischief",S,0},
    {"imp","\xf0\x9f\x91\xbf","devil angry",S,0},
    {"skull","\xf0\x9f\x92\x80","dead",S,0},
    {"skull_and_crossbones","\xe2\x98\xa0\xef\xb8\x8f","danger poison",S,0},
    {"poop","\xf0\x9f\x92\xa9","crap",S,0},
    {"clown_face","\xf0\x9f\xa4\xa1","clown",S,0},
    {"ghost","\xf0\x9f\x91\xbb","boo halloween",S,0},
    {"alien","\xf0\x9f\x91\xbd","ufo",S,0},
    {"space_invader","\xf0\x9f\x91\xbe","game alien",S,0},
    {"robot","\xf0\x9f\xa4\x96","bot",S,0},
    {"jack_o_lantern","\xf0\x9f\x8e\x83","halloween pumpkin",S,0},
    {"smiley_cat","\xf0\x9f\x98\xba","cat",S,0},
    {"smile_cat","\xf0\x9f\x98\xb8","cat",S,0},
    {"joy_cat","\xf0\x9f\x98\xb9","cat lol",S,0},
    {"heart_eyes_cat","\xf0\x9f\x98\xbb","cat love",S,0},
    {"smirk_cat","\xf0\x9f\x98\xbc","cat",S,0},
    {"kissing_cat","\xf0\x9f\x98\xbd","cat",S,0},
    {"scream_cat","\xf0\x9f\x99\x80","cat shock",S,0},
    {"crying_cat_face","\xf0\x9f\x98\xbf","cat sad",S,0},
    {"pouting_cat","\xf0\x9f\x98\xbe","cat angry",S,0},
    /* gestures */
    {"wave","\xf0\x9f\x91\x8b","hi hello bye",G,1},
    {"raised_back_of_hand","\xf0\x9f\xa4\x9a","",G,1},
    {"raised_hand_with_fingers_splayed","\xf0\x9f\x96\x90\xef\xb8\x8f","",G,1},
    {"raised_hand","\xe2\x9c\x8b","stop high five",G,1},
    {"vulcan_salute","\xf0\x9f\x96\x96","spock",G,1},
    {"ok_hand","\xf0\x9f\x91\x8c","ok perfect",G,1},
    {"pinched_fingers","\xf0\x9f\xa4\x8c","italian chef",G,1},
    {"pinching_hand","\xf0\x9f\xa4\x8f","small tiny",G,1},
    {"v","\xe2\x9c\x8c\xef\xb8\x8f","peace victory",G,1},
    {"crossed_fingers","\xf0\x9f\xa4\x9e","luck hope",G,1},
    {"love_you_gesture","\xf0\x9f\xa4\x9f","ily",G,1},
    {"metal","\xf0\x9f\xa4\x98","rock horns",G,1},
    {"call_me_hand","\xf0\x9f\xa4\x99","shaka",G,1},
    {"point_left","\xf0\x9f\x91\x88","",G,1},
    {"point_right","\xf0\x9f\x91\x89","",G,1},
    {"point_up_2","\xf0\x9f\x91\x86","",G,1},
    {"middle_finger","\xf0\x9f\x96\x95","rude",G,1},
    {"point_down","\xf0\x9f\x91\x87","",G,1},
    {"point_up","\xe2\x98\x9d\xef\xb8\x8f","",G,1},
    {"thumbsup","\xf0\x9f\x91\x8d","+1 yes like approve",G,1},
    {"thumbsdown","\xf0\x9f\x91\x8e","-1 no dislike",G,1},
    {"fist","\xe2\x9c\x8a","power solidarity",G,1},
    {"facepunch","\xf0\x9f\x91\x8a","fist bump",G,1},
    {"left_facing_fist","\xf0\x9f\xa4\x9b","bump",G,1},
    {"right_facing_fist","\xf0\x9f\xa4\x9c","bump",G,1},
    {"clap","\xf0\x9f\x91\x8f","applause bravo",G,1},
    {"raised_hands","\xf0\x9f\x99\x8c","praise celebrate hooray",G,1},
    {"open_hands","\xf0\x9f\x91\x90","",G,1},
    {"palms_up_together","\xf0\x9f\xa4\xb2","please pray",G,1},
    {"handshake","\xf0\x9f\xa4\x9d","deal agree",G,0},
    {"pray","\xf0\x9f\x99\x8f","thanks please namaste",G,1},
    {"writing_hand","\xe2\x9c\x8d\xef\xb8\x8f","write",G,1},
    {"nail_care","\xf0\x9f\x92\x85","nails fabulous",G,1},
    {"selfie","\xf0\x9f\xa4\xb3","photo",G,1},
    {"muscle","\xf0\x9f\x92\xaa","strong flex",G,1},
    /* people */
    {"baby","\xf0\x9f\x91\xb6","",P,1},
    {"child","\xf0\x9f\xa7\x92","",P,1},
    {"boy","\xf0\x9f\x91\xa6","",P,1},
    {"girl","\xf0\x9f\x91\xa7","",P,1},
    {"adult","\xf0\x9f\xa7\x91","person",P,1},
    {"man","\xf0\x9f\x91\xa8","",P,1},
    {"woman","\xf0\x9f\x91\xa9","",P,1},
    {"older_adult","\xf0\x9f\xa7\x93","elder",P,1},
    {"older_man","\xf0\x9f\x91\xb4","",P,1},
    {"older_woman","\xf0\x9f\x91\xb5","",P,1},
    {"person_frowning","\xf0\x9f\x99\x8d","",P,1},
    {"person_pouting","\xf0\x9f\x99\x8e","",P,1},
    {"no_good","\xf0\x9f\x99\x85","no refuse",P,1},
    {"ok_person","\xf0\x9f\x99\x86","yes",P,1},
    {"tipping_hand_person","\xf0\x9f\x92\x81","sassy info",P,1},
    {"raising_hand","\xf0\x9f\x99\x8b","question volunteer",P,1},
    {"deaf_person","\xf0\x9f\xa7\x8f","",P,1},
    {"bow","\xf0\x9f\x99\x87","sorry respect",P,1},
    {"facepalm","\xf0\x9f\xa4\xa6","ugh",P,1},
    {"shrug","\xf0\x9f\xa4\xb7","dunno idk",P,1},
    {"technologist","\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x92\xbb","developer engineer",P,0},
    {"mechanic","\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x94\xa7","repair",P,0},
    {"scientist","\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x94\xac","research lab",P,0},
    {"teacher","\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x8f\xab","school",P,0},
    {"judge","\xf0\x9f\xa7\x91\xe2\x80\x8d\xe2\x9a\x96\xef\xb8\x8f","law",P,0},
    {"farmer","\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x8c\xbe","agriculture",P,0},
    {"cook","\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x8d\xb3","chef",P,0},
    {"student","\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x8e\x93","graduate",P,0},
    {"singer","\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x8e\xa4","musician",P,0},
    {"artist","\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x8e\xa8","painter",P,0},
    {"pilot","\xf0\x9f\xa7\x91\xe2\x80\x8d\xe2\x9c\x88\xef\xb8\x8f","aviation",P,0},
    {"astronaut","\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x9a\x80","space",P,0},
    {"firefighter","\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x9a\x92","fire",P,0},
    {"police_officer","\xf0\x9f\x91\xae","cop",P,1},
    {"detective","\xf0\x9f\x95\xb5\xef\xb8\x8f","spy investigate",P,1},
    {"guard","\xf0\x9f\x92\x82","",P,1},
    {"construction_worker","\xf0\x9f\x91\xb7","builder",P,1},
    {"superhero","\xf0\x9f\xa6\xb8","hero",P,1},
    {"supervillain","\xf0\x9f\xa6\xb9","villain",P,1},
    {"mage","\xf0\x9f\xa7\x99","wizard",P,1},
    {"fairy","\xf0\x9f\xa7\x9a","",P,1},
    {"vampire","\xf0\x9f\xa7\x9b","",P,1},
    {"zombie","\xf0\x9f\xa7\x9f","undead",P,0},
    {"person_in_lotus_position","\xf0\x9f\xa7\x98","yoga meditate",P,1},
    {"massage","\xf0\x9f\x92\x86","spa",P,1},
    {"haircut","\xf0\x9f\x92\x87","salon",P,1},
    {"walking","\xf0\x9f\x9a\xb6","walk",P,1},
    {"running","\xf0\x9f\x8f\x83","run exercise",P,1},
    {"dancer","\xf0\x9f\x92\x83","dance",P,1},
    {"man_dancing","\xf0\x9f\x95\xba","dance",P,1},
    {"people_hugging","\xf0\x9f\xab\x82","hug support",P,0},
    {"family","\xf0\x9f\x91\xaa","",P,0},
    {"couple","\xf0\x9f\x91\xab","",P,0},
    {"bust_in_silhouette","\xf0\x9f\x91\xa4","user person",P,0},
    {"busts_in_silhouette","\xf0\x9f\x91\xa5","users people",P,0},
    {"speaking_head","\xf0\x9f\x97\xa3\xef\xb8\x8f","talk say",P,0},
    /* nature */
    {"dog","\xf0\x9f\x90\xb6","puppy",N,0},
    {"cat","\xf0\x9f\x90\xb1","kitten",N,0},
    {"mouse","\xf0\x9f\x90\xad","",N,0},
    {"hamster","\xf0\x9f\x90\xb9","",N,0},
    {"rabbit","\xf0\x9f\x90\xb0","bunny",N,0},
    {"fox_face","\xf0\x9f\xa6\x8a","fox",N,0},
    {"bear","\xf0\x9f\x90\xbb","",N,0},
    {"panda_face","\xf0\x9f\x90\xbc","panda",N,0},
    {"polar_bear","\xf0\x9f\x90\xbb\xe2\x80\x8d\xe2\x9d\x84\xef\xb8\x8f","",N,0},
    {"koala","\xf0\x9f\x90\xa8","",N,0},
    {"tiger","\xf0\x9f\x90\xaf","",N,0},
    {"lion","\xf0\x9f\xa6\x81","",N,0},
    {"cow","\xf0\x9f\x90\xae","",N,0},
    {"pig","\xf0\x9f\x90\xb7","",N,0},
    {"frog","\xf0\x9f\x90\xb8","",N,0},
    {"monkey_face","\xf0\x9f\x90\xb5","monkey",N,0},
    {"see_no_evil","\xf0\x9f\x99\x88","monkey",N,0},
    {"hear_no_evil","\xf0\x9f\x99\x89","monkey",N,0},
    {"speak_no_evil","\xf0\x9f\x99\x8a","monkey",N,0},
    {"chicken","\xf0\x9f\x90\x94","",N,0},
    {"penguin","\xf0\x9f\x90\xa7","",N,0},
    {"bird","\xf0\x9f\x90\xa6","",N,0},
    {"baby_chick","\xf0\x9f\x90\xa4","chick",N,0},
    {"eagle","\xf0\x9f\xa6\x85","",N,0},
    {"duck","\xf0\x9f\xa6\x86","",N,0},
    {"owl","\xf0\x9f\xa6\x89","",N,0},
    {"bat","\xf0\x9f\xa6\x87","",N,0},
    {"wolf","\xf0\x9f\x90\xba","",N,0},
    {"boar","\xf0\x9f\x90\x97","",N,0},
    {"horse","\xf0\x9f\x90\xb4","",N,0},
    {"unicorn","\xf0\x9f\xa6\x84","",N,0},
    {"bee","\xf0\x9f\x90\x9d","honeybee",N,0},
    {"bug","\xf0\x9f\x90\x9b","caterpillar",N,0},
    {"butterfly","\xf0\x9f\xa6\x8b","",N,0},
    {"snail","\xf0\x9f\x90\x8c","slow",N,0},
    {"lady_beetle","\xf0\x9f\x90\x9e","ladybug",N,0},
    {"ant","\xf0\x9f\x90\x9c","",N,0},
    {"spider","\xf0\x9f\x95\xb7\xef\xb8\x8f","",N,0},
    {"scorpion","\xf0\x9f\xa6\x82","",N,0},
    {"turtle","\xf0\x9f\x90\xa2","",N,0},
    {"snake","\xf0\x9f\x90\x8d","",N,0},
    {"lizard","\xf0\x9f\xa6\x8e","",N,0},
    {"t_rex","\xf0\x9f\xa6\x96","dinosaur",N,0},
    {"octopus","\xf0\x9f\x90\x99","",N,0},
    {"squid","\xf0\x9f\xa6\x91","",N,0},
    {"shrimp","\xf0\x9f\xa6\x90","",N,0},
    {"crab","\xf0\x9f\xa6\x80","",N,0},
    {"blowfish","\xf0\x9f\x90\xa1","",N,0},
    {"tropical_fish","\xf0\x9f\x90\xa0","",N,0},
    {"fish","\xf0\x9f\x90\x9f","",N,0},
    {"dolphin","\xf0\x9f\x90\xac","",N,0},
    {"whale","\xf0\x9f\x90\xb3","",N,0},
    {"shark","\xf0\x9f\xa6\x88","",N,0},
    {"crocodile","\xf0\x9f\x90\x8a","",N,0},
    {"leopard","\xf0\x9f\x90\x86","",N,0},
    {"zebra","\xf0\x9f\xa6\x93","",N,0},
    {"gorilla","\xf0\x9f\xa6\x8d","",N,0},
    {"elephant","\xf0\x9f\x90\x98","",N,0},
    {"hippopotamus","\xf0\x9f\xa6\x9b","",N,0},
    {"rhinoceros","\xf0\x9f\xa6\x8f","",N,0},
    {"camel","\xf0\x9f\x90\xab","",N,0},
    {"giraffe","\xf0\x9f\xa6\x92","",N,0},
    {"kangaroo","\xf0\x9f\xa6\x98","",N,0},
    {"sheep","\xf0\x9f\x90\x91","",N,0},
    {"goat","\xf0\x9f\x90\x90","",N,0},
    {"deer","\xf0\x9f\xa6\x8c","",N,0},
    {"hedgehog","\xf0\x9f\xa6\x94","",N,0},
    {"paw_prints","\xf0\x9f\x90\xbe","pets",N,0},
    {"dragon","\xf0\x9f\x90\x89","",N,0},
    {"cactus","\xf0\x9f\x8c\xb5","",N,0},
    {"christmas_tree","\xf0\x9f\x8e\x84","xmas",N,0},
    {"evergreen_tree","\xf0\x9f\x8c\xb2","tree",N,0},
    {"deciduous_tree","\xf0\x9f\x8c\xb3","tree",N,0},
    {"palm_tree","\xf0\x9f\x8c\xb4","",N,0},
    {"seedling","\xf0\x9f\x8c\xb1","plant sprout",N,0},
    {"herb","\xf0\x9f\x8c\xbf","leaf",N,0},
    {"shamrock","\xe2\x98\x98\xef\xb8\x8f","luck",N,0},
    {"four_leaf_clover","\xf0\x9f\x8d\x80","luck",N,0},
    {"maple_leaf","\xf0\x9f\x8d\x81","autumn",N,0},
    {"fallen_leaf","\xf0\x9f\x8d\x82","autumn",N,0},
    {"leaves","\xf0\x9f\x8d\x83","wind",N,0},
    {"mushroom","\xf0\x9f\x8d\x84","",N,0},
    {"bouquet","\xf0\x9f\x92\x90","flowers",N,0},
    {"tulip","\xf0\x9f\x8c\xb7","",N,0},
    {"rose","\xf0\x9f\x8c\xb9","",N,0},
    {"wilted_flower","\xf0\x9f\xa5\x80","dead",N,0},
    {"sunflower","\xf0\x9f\x8c\xbb","",N,0},
    {"blossom","\xf0\x9f\x8c\xbc","flower",N,0},
    {"cherry_blossom","\xf0\x9f\x8c\xb8","sakura",N,0},
    {"hibiscus","\xf0\x9f\x8c\xba","",N,0},
    {"earth_africa","\xf0\x9f\x8c\x8d","world globe",N,0},
    {"earth_americas","\xf0\x9f\x8c\x8e","world globe",N,0},
    {"earth_asia","\xf0\x9f\x8c\x8f","world globe",N,0},
    {"full_moon","\xf0\x9f\x8c\x95","",N,0},
    {"crescent_moon","\xf0\x9f\x8c\x99","night",N,0},
    {"star","\xe2\xad\x90","favorite",N,0},
    {"star2","\xf0\x9f\x8c\x9f","sparkle",N,0},
    {"sparkles","\xe2\x9c\xa8","magic shiny",N,0},
    {"zap","\xe2\x9a\xa1","lightning fast",N,0},
    {"fire","\xf0\x9f\x94\xa5","hot lit",N,0},
    {"boom","\xf0\x9f\x92\xa5","explosion",N,0},
    {"comet","\xe2\x98\x84\xef\xb8\x8f","",N,0},
    {"sunny","\xe2\x98\x80\xef\xb8\x8f","sun clear",N,0},
    {"partly_sunny","\xe2\x9b\x85","cloud",N,0},
    {"cloud","\xe2\x98\x81\xef\xb8\x8f","",N,0},
    {"rainbow","\xf0\x9f\x8c\x88","",N,0},
    {"umbrella","\xe2\x98\x94","rain",N,0},
    {"snowflake","\xe2\x9d\x84\xef\xb8\x8f","snow cold",N,0},
    {"snowman","\xe2\x9b\x84","",N,0},
    {"wind_face","\xf0\x9f\x8c\xac\xef\xb8\x8f","wind",N,0},
    {"tornado","\xf0\x9f\x8c\xaa\xef\xb8\x8f","",N,0},
    {"ocean","\xf0\x9f\x8c\x8a","wave water",N,0},
    {"droplet","\xf0\x9f\x92\xa7","water",N,0},
    {"volcano","\xf0\x9f\x8c\x8b","",N,0},
    {"mountain","\xe2\x9b\xb0\xef\xb8\x8f","",N,0},
    {"desert","\xf0\x9f\x8f\x9c\xef\xb8\x8f","",N,0},
    {"beach_umbrella","\xf0\x9f\x8f\x96\xef\xb8\x8f","beach",N,0},
    /* food */
    {"green_apple","\xf0\x9f\x8d\x8f","",F,0},
    {"apple","\xf0\x9f\x8d\x8e","",F,0},
    {"pear","\xf0\x9f\x8d\x90","",F,0},
    {"tangerine","\xf0\x9f\x8d\x8a","orange",F,0},
    {"lemon","\xf0\x9f\x8d\x8b","",F,0},
    {"banana","\xf0\x9f\x8d\x8c","",F,0},
    {"watermelon","\xf0\x9f\x8d\x89","",F,0},
    {"grapes","\xf0\x9f\x8d\x87","",F,0},
    {"strawberry","\xf0\x9f\x8d\x93","",F,0},
    {"blueberries","\xf0\x9f\xab\x90","",F,0},
    {"melon","\xf0\x9f\x8d\x88","",F,0},
    {"cherries","\xf0\x9f\x8d\x92","",F,0},
    {"peach","\xf0\x9f\x8d\x91","",F,0},
    {"mango","\xf0\x9f\xa5\xad","",F,0},
    {"pineapple","\xf0\x9f\x8d\x8d","",F,0},
    {"coconut","\xf0\x9f\xa5\xa5","",F,0},
    {"kiwi_fruit","\xf0\x9f\xa5\x9d","kiwi",F,0},
    {"tomato","\xf0\x9f\x8d\x85","",F,0},
    {"eggplant","\xf0\x9f\x8d\x86","",F,0},
    {"avocado","\xf0\x9f\xa5\x91","",F,0},
    {"broccoli","\xf0\x9f\xa5\xa6","",F,0},
    {"leafy_green","\xf0\x9f\xa5\xac","lettuce",F,0},
    {"cucumber","\xf0\x9f\xa5\x92","",F,0},
    {"hot_pepper","\xf0\x9f\x8c\xb6\xef\xb8\x8f","spicy",F,0},
    {"corn","\xf0\x9f\x8c\xbd","",F,0},
    {"carrot","\xf0\x9f\xa5\x95","",F,0},
    {"garlic","\xf0\x9f\xa7\x84","",F,0},
    {"onion","\xf0\x9f\xa7\x85","",F,0},
    {"potato","\xf0\x9f\xa5\x94","",F,0},
    {"sweet_potato","\xf0\x9f\x8d\xa0","",F,0},
    {"croissant","\xf0\x9f\xa5\x90","",F,0},
    {"bagel","\xf0\x9f\xa5\xaf","",F,0},
    {"bread","\xf0\x9f\x8d\x9e","",F,0},
    {"baguette_bread","\xf0\x9f\xa5\x96","baguette",F,0},
    {"pretzel","\xf0\x9f\xa5\xa8","",F,0},
    {"cheese","\xf0\x9f\xa7\x80","",F,0},
    {"egg","\xf0\x9f\xa5\x9a","",F,0},
    {"cooking","\xf0\x9f\x8d\xb3","fried egg",F,0},
    {"butter","\xf0\x9f\xa7\x88","",F,0},
    {"pancakes","\xf0\x9f\xa5\x9e","",F,0},
    {"waffle","\xf0\x9f\xa7\x87","",F,0},
    {"bacon","\xf0\x9f\xa5\x93","",F,0},
    {"cut_of_meat","\xf0\x9f\xa5\xa9","steak",F,0},
    {"poultry_leg","\xf0\x9f\x8d\x97","chicken",F,0},
    {"meat_on_bone","\xf0\x9f\x8d\x96","",F,0},
    {"hotdog","\xf0\x9f\x8c\xad","hot dog",F,0},
    {"hamburger","\xf0\x9f\x8d\x94","burger",F,0},
    {"fries","\xf0\x9f\x8d\x9f","chips",F,0},
    {"pizza","\xf0\x9f\x8d\x95","",F,0},
    {"sandwich","\xf0\x9f\xa5\xaa","",F,0},
    {"taco","\xf0\x9f\x8c\xae","",F,0},
    {"burrito","\xf0\x9f\x8c\xaf","",F,0},
    {"stuffed_flatbread","\xf0\x9f\xa5\x99","kebab",F,0},
    {"falafel","\xf0\x9f\xa7\x86","",F,0},
    {"salad","\xf0\x9f\xa5\x97","",F,0},
    {"shallow_pan_of_food","\xf0\x9f\xa5\x98","paella",F,0},
    {"stew","\xf0\x9f\x8d\xb2","",F,0},
    {"bowl_with_spoon","\xf0\x9f\xa5\xa3","cereal",F,0},
    {"canned_food","\xf0\x9f\xa5\xab","",F,0},
    {"spaghetti","\xf0\x9f\x8d\x9d","pasta",F,0},
    {"ramen","\xf0\x9f\x8d\x9c","noodles",F,0},
    {"curry","\xf0\x9f\x8d\x9b","",F,0},
    {"sushi","\xf0\x9f\x8d\xa3","",F,0},
    {"bento","\xf0\x9f\x8d\xb1","",F,0},
    {"dumpling","\xf0\x9f\xa5\x9f","",F,0},
    {"fortune_cookie","\xf0\x9f\xa5\xa0","",F,0},
    {"rice","\xf0\x9f\x8d\x9a","",F,0},
    {"rice_ball","\xf0\x9f\x8d\x99","",F,0},
    {"oden","\xf0\x9f\x8d\xa2","",F,0},
    {"fish_cake","\xf0\x9f\x8d\xa5","",F,0},
    {"shaved_ice","\xf0\x9f\x8d\xa7","",F,0},
    {"ice_cream","\xf0\x9f\x8d\xa8","",F,0},
    {"icecream","\xf0\x9f\x8d\xa6","soft serve",F,0},
    {"doughnut","\xf0\x9f\x8d\xa9","donut",F,0},
    {"cookie","\xf0\x9f\x8d\xaa","",F,0},
    {"popcorn","\xf0\x9f\x8d\xbf","movie snack",F,0},
    {"birthday","\xf0\x9f\x8e\x82","cake celebrate",F,0},
    {"cake","\xf0\x9f\x8d\xb0","dessert",F,0},
    {"cupcake","\xf0\x9f\xa7\x81","",F,0},
    {"pie","\xf0\x9f\xa5\xa7","",F,0},
    {"chocolate_bar","\xf0\x9f\x8d\xab","chocolate",F,0},
    {"candy","\xf0\x9f\x8d\xac","",F,0},
    {"lollipop","\xf0\x9f\x8d\xad","",F,0},
    {"honey_pot","\xf0\x9f\x8d\xaf","honey",F,0},
    {"baby_bottle","\xf0\x9f\x8d\xbc","",F,0},
    {"milk_glass","\xf0\x9f\xa5\x9b","milk",F,0},
    {"coffee","\xe2\x98\x95","tea espresso",F,0},
    {"teapot","\xf0\x9f\xab\x96","",F,0},
    {"tea","\xf0\x9f\x8d\xb5","green tea",F,0},
    {"sake","\xf0\x9f\x8d\xb6","",F,0},
    {"champagne","\xf0\x9f\x8d\xbe","celebrate",F,0},
    {"wine_glass","\xf0\x9f\x8d\xb7","wine",F,0},
    {"cocktail","\xf0\x9f\x8d\xb8","martini",F,0},
    {"tropical_drink","\xf0\x9f\x8d\xb9","",F,0},
    {"beer","\xf0\x9f\x8d\xba","",F,0},
    {"beers","\xf0\x9f\x8d\xbb","cheers",F,0},
    {"clinking_glasses","\xf0\x9f\xa5\x82","cheers toast",F,0},
    {"tumbler_glass","\xf0\x9f\xa5\x83","whisky",F,0},
    {"cup_with_straw","\xf0\x9f\xa5\xa4","soda",F,0},
    {"bubble_tea","\xf0\x9f\xa7\x8b","boba",F,0},
    {"mate","\xf0\x9f\xa7\x89","",F,0},
    {"ice_cube","\xf0\x9f\xa7\x8a","ice",F,0},
    {"chopsticks","\xf0\x9f\xa5\xa2","",F,0},
    {"fork_and_knife","\xf0\x9f\x8d\xb4","eat",F,0},
    {"plate_with_cutlery","\xf0\x9f\x8d\xbd\xef\xb8\x8f","dinner",F,0},
    {"spoon","\xf0\x9f\xa5\x84","",F,0},
    {"salt","\xf0\x9f\xa7\x82","",F,0},
    /* activity */
    {"soccer","\xe2\x9a\xbd","football",A,0},
    {"basketball","\xf0\x9f\x8f\x80","",A,0},
    {"football","\xf0\x9f\x8f\x88","american football",A,0},
    {"baseball","\xe2\x9a\xbe","",A,0},
    {"softball","\xf0\x9f\xa5\x8e","",A,0},
    {"tennis","\xf0\x9f\x8e\xbe","",A,0},
    {"volleyball","\xf0\x9f\x8f\x90","",A,0},
    {"rugby_football","\xf0\x9f\x8f\x89","rugby",A,0},
    {"flying_disc","\xf0\x9f\xa5\x8f","frisbee",A,0},
    {"8ball","\xf0\x9f\x8e\xb1","pool billiards",A,0},
    {"bowling","\xf0\x9f\x8e\xb3","",A,0},
    {"cricket_game","\xf0\x9f\x8f\x8f","cricket",A,0},
    {"field_hockey","\xf0\x9f\x8f\x91","",A,0},
    {"ice_hockey","\xf0\x9f\x8f\x92","",A,0},
    {"lacrosse","\xf0\x9f\xa5\x8d","",A,0},
    {"ping_pong","\xf0\x9f\x8f\x93","table tennis",A,0},
    {"badminton","\xf0\x9f\x8f\xb8","",A,0},
    {"boxing_glove","\xf0\x9f\xa5\x8a","boxing",A,0},
    {"martial_arts_uniform","\xf0\x9f\xa5\x8b","karate judo",A,0},
    {"goal_net","\xf0\x9f\xa5\x85","goal",A,0},
    {"golf","\xe2\x9b\xb3","golfing",A,0},
    {"ice_skate","\xe2\x9b\xb8\xef\xb8\x8f","skating",A,0},
    {"fishing_pole_and_fish","\xf0\x9f\x8e\xa3","fishing",A,0},
    {"diving_mask","\xf0\x9f\xa4\xbf","diving",A,0},
    {"running_shirt_with_sash","\xf0\x9f\x8e\xbd","marathon",A,0},
    {"ski","\xf0\x9f\x8e\xbf","skiing",A,0},
    {"sled","\xf0\x9f\x9b\xb7","",A,0},
    {"curling_stone","\xf0\x9f\xa5\x8c","curling",A,0},
    {"dart","\xf0\x9f\x8e\xaf","target bullseye",A,0},
    {"yo_yo","\xf0\x9f\xaa\x80","",A,0},
    {"kite","\xf0\x9f\xaa\x81","",A,0},
    {"crystal_ball","\xf0\x9f\x94\xae","fortune magic",A,0},
    {"video_game","\xf0\x9f\x8e\xae","gaming",A,0},
    {"joystick","\xf0\x9f\x95\xb9\xef\xb8\x8f","",A,0},
    {"slot_machine","\xf0\x9f\x8e\xb0","gambling",A,0},
    {"game_die","\xf0\x9f\x8e\xb2","dice random",A,0},
    {"jigsaw","\xf0\x9f\xa7\xa9","puzzle",A,0},
    {"teddy_bear","\xf0\x9f\xa7\xb8","",A,0},
    {"chess_pawn","\xe2\x99\x9f\xef\xb8\x8f","chess",A,0},
    {"performing_arts","\xf0\x9f\x8e\xad","theater drama",A,0},
    {"art","\xf0\x9f\x8e\xa8","paint palette",A,0},
    {"thread","\xf0\x9f\xa7\xb5","sewing",A,0},
    {"yarn","\xf0\x9f\xa7\xb6","knitting",A,0},
    {"microphone","\xf0\x9f\x8e\xa4","sing karaoke",A,0},
    {"headphones","\xf0\x9f\x8e\xa7","music listen",A,0},
    {"musical_score","\xf0\x9f\x8e\xbc","music",A,0},
    {"musical_note","\xf0\x9f\x8e\xb5","music",A,0},
    {"notes","\xf0\x9f\x8e\xb6","music",A,0},
    {"saxophone","\xf0\x9f\x8e\xb7","",A,0},
    {"guitar","\xf0\x9f\x8e\xb8","",A,0},
    {"musical_keyboard","\xf0\x9f\x8e\xb9","piano",A,0},
    {"trumpet","\xf0\x9f\x8e\xba","",A,0},
    {"violin","\xf0\x9f\x8e\xbb","",A,0},
    {"drum","\xf0\x9f\xa5\x81","",A,0},
    {"banjo","\xf0\x9f\xaa\x95","",A,0},
    {"clapper","\xf0\x9f\x8e\xac","movie action",A,0},
    {"bow_and_arrow","\xf0\x9f\x8f\xb9","archery",A,0},
    {"trophy","\xf0\x9f\x8f\x86","win champion",A,0},
    {"first_place_medal","\xf0\x9f\xa5\x87","gold win",A,0},
    {"second_place_medal","\xf0\x9f\xa5\x88","silver",A,0},
    {"third_place_medal","\xf0\x9f\xa5\x89","bronze",A,0},
    {"medal_sports","\xf0\x9f\x8f\x85","medal",A,0},
    {"medal_military","\xf0\x9f\x8e\x96\xef\xb8\x8f","",A,0},
    {"ticket","\xf0\x9f\x8e\xab","",A,0},
    {"tickets","\xf0\x9f\x8e\x9f\xef\xb8\x8f","",A,0},
    {"circus_tent","\xf0\x9f\x8e\xaa","",A,0},
    {"carousel_horse","\xf0\x9f\x8e\xa0","",A,0},
    {"ferris_wheel","\xf0\x9f\x8e\xa1","",A,0},
    {"roller_coaster","\xf0\x9f\x8e\xa2","",A,0},
    {"balloon","\xf0\x9f\x8e\x88","party",A,0},
    {"tada","\xf0\x9f\x8e\x89","party celebrate hooray",A,0},
    {"confetti_ball","\xf0\x9f\x8e\x8a","party",A,0},
    {"gift","\xf0\x9f\x8e\x81","present",A,0},
    {"ribbon","\xf0\x9f\x8e\x80","",A,0},
    {"sparkler","\xf0\x9f\x8e\x87","",A,0},
    {"fireworks","\xf0\x9f\x8e\x86","",A,0},
    {"red_envelope","\xf0\x9f\xa7\xa7","",A,0},
    /* objects */
    {"watch","\xe2\x8c\x9a","time",O,0},
    {"iphone","\xf0\x9f\x93\xb1","phone mobile",O,0},
    {"computer","\xf0\x9f\x92\xbb","laptop",O,0},
    {"brain","\xf0\x9f\xa7\xa0","think smart",O,0},
    {"pushpin","\xf0\x9f\x93\x8c","pin",O,0},
    {"desktop_computer","\xf0\x9f\x96\xa5\xef\xb8\x8f","monitor",O,0},
    {"keyboard","\xe2\x8c\xa8\xef\xb8\x8f","",O,0},
    {"printer","\xf0\x9f\x96\xa8\xef\xb8\x8f","",O,0},
    {"computer_mouse","\xf0\x9f\x96\xb1\xef\xb8\x8f","mouse",O,0},
    {"floppy_disk","\xf0\x9f\x92\xbe","save",O,0},
    {"cd","\xf0\x9f\x92\xbf","disc",O,0},
    {"dvd","\xf0\x9f\x93\x80","",O,0},
    {"minidisc","\xf0\x9f\x92\xbd","",O,0},
    {"vhs","\xf0\x9f\x93\xbc","tape",O,0},
    {"camera","\xf0\x9f\x93\xb7","photo",O,0},
    {"camera_flash","\xf0\x9f\x93\xb8","photo",O,0},
    {"video_camera","\xf0\x9f\x93\xb9","",O,0},
    {"movie_camera","\xf0\x9f\x8e\xa5","film",O,0},
    {"film_projector","\xf0\x9f\x93\xbd\xef\xb8\x8f","",O,0},
    {"telephone_receiver","\xf0\x9f\x93\x9e","call",O,0},
    {"pager","\xf0\x9f\x93\x9f","",O,0},
    {"fax","\xf0\x9f\x93\xa0","",O,0},
    {"tv","\xf0\x9f\x93\xba","television",O,0},
    {"radio","\xf0\x9f\x93\xbb","",O,0},
    {"studio_microphone","\xf0\x9f\x8e\x99\xef\xb8\x8f","podcast",O,0},
    {"level_slider","\xf0\x9f\x8e\x9a\xef\xb8\x8f","",O,0},
    {"control_knobs","\xf0\x9f\x8e\x9b\xef\xb8\x8f","",O,0},
    {"compass","\xf0\x9f\xa7\xad","direction",O,0},
    {"stopwatch","\xe2\x8f\xb1\xef\xb8\x8f","timer",O,0},
    {"timer_clock","\xe2\x8f\xb2\xef\xb8\x8f","timer",O,0},
    {"alarm_clock","\xe2\x8f\xb0","alarm",O,0},
    {"hourglass","\xe2\x8c\x9b","time waiting",O,0},
    {"hourglass_flowing_sand","\xe2\x8f\xb3","time waiting",O,0},
    {"satellite","\xf0\x9f\x9b\xb0\xef\xb8\x8f","space orbit",O,0},
    {"battery","\xf0\x9f\x94\x8b","power",O,0},
    {"electric_plug","\xf0\x9f\x94\x8c","power",O,0},
    {"bulb","\xf0\x9f\x92\xa1","idea light",O,0},
    {"flashlight","\xf0\x9f\x94\xa6","torch",O,0},
    {"candle","\xf0\x9f\x95\xaf\xef\xb8\x8f","",O,0},
    {"wastebasket","\xf0\x9f\x97\x91\xef\xb8\x8f","trash delete bin",O,0},
    {"oil_drum","\xf0\x9f\x9b\xa2\xef\xb8\x8f","",O,0},
    {"money_with_wings","\xf0\x9f\x92\xb8","spend",O,0},
    {"dollar","\xf0\x9f\x92\xb5","money cash",O,0},
    {"yen","\xf0\x9f\x92\xb4","money",O,0},
    {"euro","\xf0\x9f\x92\xb6","money",O,0},
    {"pound","\xf0\x9f\x92\xb7","money",O,0},
    {"moneybag","\xf0\x9f\x92\xb0","money rich",O,0},
    {"credit_card","\xf0\x9f\x92\xb3","payment",O,0},
    {"gem","\xf0\x9f\x92\x8e","diamond jewel",O,0},
    {"balance_scale","\xe2\x9a\x96\xef\xb8\x8f","justice law",O,0},
    {"ladder","\xf0\x9f\xaa\x9c","",O,0},
    {"toolbox","\xf0\x9f\xa7\xb0","tools",O,0},
    {"wrench","\xf0\x9f\x94\xa7","fix tool",O,0},
    {"hammer","\xf0\x9f\x94\xa8","build tool",O,0},
    {"hammer_and_wrench","\xf0\x9f\x9b\xa0\xef\xb8\x8f","tools build",O,0},
    {"screwdriver","\xf0\x9f\xaa\x9b","",O,0},
    {"nut_and_bolt","\xf0\x9f\x94\xa9","",O,0},
    {"gear","\xe2\x9a\x99\xef\xb8\x8f","settings config",O,0},
    {"clamp","\xf0\x9f\x97\x9c\xef\xb8\x8f","",O,0},
    {"link","\xf0\x9f\x94\x97","url",O,0},
    {"chains","\xe2\x9b\x93\xef\xb8\x8f","",O,0},
    {"hook","\xf0\x9f\xaa\x9d","",O,0},
    {"magnet","\xf0\x9f\xa7\xb2","",O,0},
    {"test_tube","\xf0\x9f\xa7\xaa","science experiment",O,0},
    {"petri_dish","\xf0\x9f\xa7\xab","science",O,0},
    {"dna","\xf0\x9f\xa7\xac","genetics",O,0},
    {"microscope","\xf0\x9f\x94\xac","science",O,0},
    {"telescope","\xf0\x9f\x94\xad","space",O,0},
    {"satellite_antenna","\xf0\x9f\x93\xa1","signal",O,0},
    {"syringe","\xf0\x9f\x92\x89","vaccine shot",O,0},
    {"pill","\xf0\x9f\x92\x8a","medicine",O,0},
    {"stethoscope","\xf0\x9f\xa9\xba","doctor health",O,0},
    {"bandage","\xf0\x9f\xa9\xb9","",O,0},
    {"door","\xf0\x9f\x9a\xaa","",O,0},
    {"bed","\xf0\x9f\x9b\x8f\xef\xb8\x8f","sleep",O,0},
    {"couch_and_lamp","\xf0\x9f\x9b\x8b\xef\xb8\x8f","sofa",O,0},
    {"chair","\xf0\x9f\xaa\x91","",O,0},
    {"toilet","\xf0\x9f\x9a\xbd","",O,0},
    {"shower","\xf0\x9f\x9a\xbf","",O,0},
    {"bathtub","\xf0\x9f\x9b\x81","",O,0},
    {"soap","\xf0\x9f\xa7\xbc","wash",O,0},
    {"sponge","\xf0\x9f\xa7\xbd","",O,0},
    {"broom","\xf0\x9f\xa7\xb9","clean sweep",O,0},
    {"basket","\xf0\x9f\xa7\xba","",O,0},
    {"roll_of_paper","\xf0\x9f\xa7\xbb","toilet paper",O,0},
    {"bucket","\xf0\x9f\xaa\xa3","",O,0},
    {"key","\xf0\x9f\x94\x91","password unlock",O,0},
    {"lock","\xf0\x9f\x94\x92","secure private",O,0},
    {"unlock","\xf0\x9f\x94\x93","open",O,0},
    {"closed_lock_with_key","\xf0\x9f\x94\x90","secure",O,0},
    {"shield","\xf0\x9f\x9b\xa1\xef\xb8\x8f","security protect",O,0},
    {"package","\xf0\x9f\x93\xa6","box shipping",O,0},
    {"mailbox","\xf0\x9f\x93\xab","mail",O,0},
    {"envelope","\xe2\x9c\x89\xef\xb8\x8f","mail email",O,0},
    {"email","\xf0\x9f\x93\xa7","mail",O,0},
    {"incoming_envelope","\xf0\x9f\x93\xa8","mail",O,0},
    {"outbox_tray","\xf0\x9f\x93\xa4","send",O,0},
    {"inbox_tray","\xf0\x9f\x93\xa5","receive",O,0},
    {"postbox","\xf0\x9f\x93\xae","mail",O,0},
    {"memo","\xf0\x9f\x93\x9d","note write edit",O,0},
    {"page_facing_up","\xf0\x9f\x93\x84","document file",O,0},
    {"page_with_curl","\xf0\x9f\x93\x83","document",O,0},
    {"bookmark_tabs","\xf0\x9f\x93\x91","",O,0},
    {"scroll","\xf0\x9f\x93\x9c","document",O,0},
    {"clipboard","\xf0\x9f\x93\x8b","copy list",O,0},
    {"calendar","\xf0\x9f\x93\x85","date schedule",O,0},
    {"spiral_calendar","\xf0\x9f\x97\x93\xef\xb8\x8f","schedule",O,0},
    {"date","\xf0\x9f\x93\x86","calendar",O,0},
    {"card_index_dividers","\xf0\x9f\x97\x82\xef\xb8\x8f","files folders",O,0},
    {"file_folder","\xf0\x9f\x93\x81","folder directory",O,0},
    {"open_file_folder","\xf0\x9f\x93\x82","folder",O,0},
    {"card_file_box","\xf0\x9f\x97\x83\xef\xb8\x8f","archive",O,0},
    {"file_cabinet","\xf0\x9f\x97\x84\xef\xb8\x8f","storage",O,0},
    {"newspaper","\xf0\x9f\x93\xb0","news",O,0},
    {"book","\xf0\x9f\x93\x96","read",O,0},
    {"books","\xf0\x9f\x93\x9a","library reading",O,0},
    {"notebook","\xf0\x9f\x93\x93","",O,0},
    {"ledger","\xf0\x9f\x93\x92","",O,0},
    {"closed_book","\xf0\x9f\x93\x95","",O,0},
    {"green_book","\xf0\x9f\x93\x97","",O,0},
    {"blue_book","\xf0\x9f\x93\x98","",O,0},
    {"orange_book","\xf0\x9f\x93\x99","",O,0},
    {"bookmark","\xf0\x9f\x94\x96","save",O,0},
    {"label","\xf0\x9f\x8f\xb7\xef\xb8\x8f","tag",O,0},
    {"paperclip","\xf0\x9f\x93\x8e","attachment",O,0},
    {"paperclips","\xf0\x9f\x96\x87\xef\xb8\x8f","attachments",O,0},
    {"straight_ruler","\xf0\x9f\x93\x8f","measure",O,0},
    {"triangular_ruler","\xf0\x9f\x93\x90","measure",O,0},
    {"scissors","\xe2\x9c\x82\xef\xb8\x8f","cut",O,0},
    {"pen","\xf0\x9f\x96\x8a\xef\xb8\x8f","write",O,0},
    {"fountain_pen","\xf0\x9f\x96\x8b\xef\xb8\x8f","write",O,0},
    {"pencil2","\xe2\x9c\x8f\xef\xb8\x8f","write edit",O,0},
    {"crayon","\xf0\x9f\x96\x8d\xef\xb8\x8f","",O,0},
    {"paintbrush","\xf0\x9f\x96\x8c\xef\xb8\x8f","paint",O,0},
    {"mag","\xf0\x9f\x94\x8d","search find zoom",O,0},
    {"mag_right","\xf0\x9f\x94\x8e","search",O,0},
    {"bar_chart","\xf0\x9f\x93\x8a","stats analytics",O,0},
    {"chart_with_upwards_trend","\xf0\x9f\x93\x88","growth up",O,0},
    {"chart_with_downwards_trend","\xf0\x9f\x93\x89","decline down",O,0},
    {"chart","\xf0\x9f\x92\xb9","money chart",O,0},
    {"abacus","\xf0\x9f\xa7\xae","math count",O,0},
    {"bell","\xf0\x9f\x94\x94","notification alert",O,0},
    {"no_bell","\xf0\x9f\x94\x95","mute silence",O,0},
    {"loudspeaker","\xf0\x9f\x93\xa2","announce",O,0},
    {"mega","\xf0\x9f\x93\xa3","announce shout",O,0},
    {"postal_horn","\xf0\x9f\x93\xaf","",O,0},
    {"cinema","\xf0\x9f\x8e\xa6","movie",O,0},
    {"bomb","\xf0\x9f\x92\xa3","explosive",O,0},
    {"hole","\xf0\x9f\x95\xb3\xef\xb8\x8f","",O,0},
    {"thermometer","\xf0\x9f\x8c\xa1\xef\xb8\x8f","temperature",O,0},
    {"world_map","\xf0\x9f\x97\xba\xef\xb8\x8f","map",O,0},
    {"rocket","\xf0\x9f\x9a\x80","launch ship fast",O,0},
    {"airplane","\xe2\x9c\x88\xef\xb8\x8f","flight travel",O,0},
    {"helicopter","\xf0\x9f\x9a\x81","",O,0},
    {"car","\xf0\x9f\x9a\x97","auto",O,0},
    {"taxi","\xf0\x9f\x9a\x95","",O,0},
    {"bus","\xf0\x9f\x9a\x8c","",O,0},
    {"truck","\xf0\x9f\x9a\x9a","delivery",O,0},
    {"bike","\xf0\x9f\x9a\xb2","bicycle",O,0},
    {"scooter","\xf0\x9f\x9b\xb4","",O,0},
    {"motorcycle","\xf0\x9f\x8f\x8d\xef\xb8\x8f","",O,0},
    {"train","\xf0\x9f\x9a\x86","rail",O,0},
    {"metro","\xf0\x9f\x9a\x87","subway",O,0},
    {"tram","\xf0\x9f\x9a\x8a","",O,0},
    {"ship","\xf0\x9f\x9a\xa2","boat",O,0},
    {"sailboat","\xe2\x9b\xb5","boat",O,0},
    {"speedboat","\xf0\x9f\x9a\xa4","boat",O,0},
    {"anchor","\xe2\x9a\x93","",O,0},
    {"construction","\xf0\x9f\x9a\xa7","wip roadwork",O,0},
    {"traffic_light","\xf0\x9f\x9a\xa6","",O,0},
    {"house","\xf0\x9f\x8f\xa0","home",O,0},
    {"office","\xf0\x9f\x8f\xa2","building work",O,0},
    {"hospital","\xf0\x9f\x8f\xa5","",O,0},
    {"bank","\xf0\x9f\x8f\xa6","",O,0},
    {"school","\xf0\x9f\x8f\xab","",O,0},
    {"factory","\xf0\x9f\x8f\xad","",O,0},
    {"hotel","\xf0\x9f\x8f\xa8","",O,0},
    {"church","\xe2\x9b\xaa","",O,0},
    {"stadium","\xf0\x9f\x8f\x9f\xef\xb8\x8f","",O,0},
    {"classical_building","\xf0\x9f\x8f\x9b\xef\xb8\x8f","museum",O,0},
    {"statue_of_liberty","\xf0\x9f\x97\xbd","",O,0},
    {"bridge_at_night","\xf0\x9f\x8c\x89","bridge",O,0},
    {"city_sunset","\xf0\x9f\x8c\x87","city",O,0},
    {"night_with_stars","\xf0\x9f\x8c\x83","city night",O,0},
    {"cityscape","\xf0\x9f\x8f\x99\xef\xb8\x8f","city skyline",O,0},
    {"tent","\xe2\x9b\xba","camping",O,0},
    {"national_park","\xf0\x9f\x8f\x9e\xef\xb8\x8f","nature",O,0},
    {"mount_fuji","\xf0\x9f\x97\xbb","",O,0},
    /* symbols */
    {"heart","\xe2\x9d\xa4\xef\xb8\x8f","love",Y,0},
    {"orange_heart","\xf0\x9f\xa7\xa1","love",Y,0},
    {"yellow_heart","\xf0\x9f\x92\x9b","love",Y,0},
    {"green_heart","\xf0\x9f\x92\x9a","love",Y,0},
    {"blue_heart","\xf0\x9f\x92\x99","love",Y,0},
    {"purple_heart","\xf0\x9f\x92\x9c","love",Y,0},
    {"black_heart","\xf0\x9f\x96\xa4","love",Y,0},
    {"white_heart","\xf0\x9f\xa4\x8d","love",Y,0},
    {"brown_heart","\xf0\x9f\xa4\x8e","love",Y,0},
    {"broken_heart","\xf0\x9f\x92\x94","sad heartbreak",Y,0},
    {"heart_on_fire","\xe2\x9d\xa4\xef\xb8\x8f\xe2\x80\x8d\xf0\x9f\x94\xa5","passion",Y,0},
    {"mending_heart","\xe2\x9d\xa4\xef\xb8\x8f\xe2\x80\x8d\xf0\x9f\xa9\xb9","healing",Y,0},
    {"two_hearts","\xf0\x9f\x92\x95","love",Y,0},
    {"sparkling_heart","\xf0\x9f\x92\x96","love",Y,0},
    {"heartpulse","\xf0\x9f\x92\x97","love",Y,0},
    {"heartbeat","\xf0\x9f\x92\x93","love",Y,0},
    {"revolving_hearts","\xf0\x9f\x92\x9e","love",Y,0},
    {"cupid","\xf0\x9f\x92\x98","love arrow",Y,0},
    {"gift_heart","\xf0\x9f\x92\x9d","love",Y,0},
    {"100","\xf0\x9f\x92\xaf","perfect score hundred",Y,0},
    {"anger","\xf0\x9f\x92\xa2","angry",Y,0},
    {"collision","\xf0\x9f\x92\xa5","boom",Y,0},
    {"dizzy","\xf0\x9f\x92\xab","star",Y,0},
    {"sweat_drops","\xf0\x9f\x92\xa6","water splash",Y,0},
    {"dash","\xf0\x9f\x92\xa8","fast wind",Y,0},
    {"speech_balloon","\xf0\x9f\x92\xac","comment talk",Y,0},
    {"left_speech_bubble","\xf0\x9f\x97\xa8\xef\xb8\x8f","comment",Y,0},
    {"right_anger_bubble","\xf0\x9f\x97\xaf\xef\xb8\x8f","angry",Y,0},
    {"thought_balloon","\xf0\x9f\x92\xad","thinking",Y,0},
    {"zzz","\xf0\x9f\x92\xa4","sleep idle",Y,0},
    {"check_mark","\xe2\x9c\x94\xef\xb8\x8f","done yes",Y,0},
    {"white_check_mark","\xe2\x9c\x85","done yes pass",Y,0},
    {"ballot_box_with_check","\xe2\x98\x91\xef\xb8\x8f","done",Y,0},
    {"heavy_check_mark","\xe2\x9c\x94\xef\xb8\x8f","done",Y,0},
    {"x","\xe2\x9d\x8c","no fail wrong",Y,0},
    {"negative_squared_cross_mark","\xe2\x9d\x8e","no",Y,0},
    {"heavy_plus_sign","\xe2\x9e\x95","add plus",Y,0},
    {"heavy_minus_sign","\xe2\x9e\x96","minus remove",Y,0},
    {"heavy_division_sign","\xe2\x9e\x97","divide",Y,0},
    {"heavy_multiplication_x","\xe2\x9c\x96\xef\xb8\x8f","times",Y,0},
    {"infinity","\xe2\x99\xbe\xef\xb8\x8f","forever",Y,0},
    {"bangbang","\xe2\x80\xbc\xef\xb8\x8f","exclamation",Y,0},
    {"interrobang","\xe2\x81\x89\xef\xb8\x8f","",Y,0},
    {"question","\xe2\x9d\x93","help unknown",Y,0},
    {"grey_question","\xe2\x9d\x94","help",Y,0},
    {"exclamation","\xe2\x9d\x97","warning important",Y,0},
    {"grey_exclamation","\xe2\x9d\x95","",Y,0},
    {"warning","\xe2\x9a\xa0\xef\xb8\x8f","caution danger",Y,0},
    {"no_entry","\xe2\x9b\x94","blocked stop",Y,0},
    {"no_entry_sign","\xf0\x9f\x9a\xab","forbidden banned",Y,0},
    {"radioactive","\xe2\x98\xa2\xef\xb8\x8f","danger",Y,0},
    {"biohazard","\xe2\x98\xa3\xef\xb8\x8f","danger",Y,0},
    {"recycle","\xe2\x99\xbb\xef\xb8\x8f","retry reuse",Y,0},
    {"arrow_up","\xe2\xac\x86\xef\xb8\x8f","",Y,0},
    {"arrow_down","\xe2\xac\x87\xef\xb8\x8f","",Y,0},
    {"arrow_left","\xe2\xac\x85\xef\xb8\x8f","",Y,0},
    {"arrow_right","\xe2\x9e\xa1\xef\xb8\x8f","",Y,0},
    {"arrow_upper_right","\xe2\x86\x97\xef\xb8\x8f","",Y,0},
    {"arrow_lower_right","\xe2\x86\x98\xef\xb8\x8f","",Y,0},
    {"arrow_lower_left","\xe2\x86\x99\xef\xb8\x8f","",Y,0},
    {"arrow_upper_left","\xe2\x86\x96\xef\xb8\x8f","",Y,0},
    {"left_right_arrow","\xe2\x86\x94\xef\xb8\x8f","",Y,0},
    {"arrow_up_down","\xe2\x86\x95\xef\xb8\x8f","",Y,0},
    {"arrows_counterclockwise","\xf0\x9f\x94\x84","refresh sync retry",Y,0},
    {"arrows_clockwise","\xf0\x9f\x94\x83","refresh",Y,0},
    {"arrow_right_hook","\xe2\x86\xaa\xef\xb8\x8f","reply forward",Y,0},
    {"leftwards_arrow_with_hook","\xe2\x86\xa9\xef\xb8\x8f","reply back",Y,0},
    {"arrow_forward","\xe2\x96\xb6\xef\xb8\x8f","play",Y,0},
    {"arrow_backward","\xe2\x97\x80\xef\xb8\x8f","rewind",Y,0},
    {"fast_forward","\xe2\x8f\xa9","",Y,0},
    {"rewind","\xe2\x8f\xaa","",Y,0},
    {"black_right_pointing_double_triangle_with_vertical_bar","\xe2\x8f\xad\xef\xb8\x8f","next",Y,0},
    {"black_left_pointing_double_triangle_with_vertical_bar","\xe2\x8f\xae\xef\xb8\x8f","previous",Y,0},
    {"double_vertical_bar","\xe2\x8f\xb8\xef\xb8\x8f","pause",Y,0},
    {"black_square_for_stop","\xe2\x8f\xb9\xef\xb8\x8f","stop",Y,0},
    {"record_button","\xe2\x8f\xba\xef\xb8\x8f","record",Y,0},
    {"eject_button","\xe2\x8f\x8f\xef\xb8\x8f","eject",Y,0},
    {"repeat","\xf0\x9f\x94\x81","loop",Y,0},
    {"repeat_one","\xf0\x9f\x94\x82","",Y,0},
    {"twisted_rightwards_arrows","\xf0\x9f\x94\x80","shuffle",Y,0},
    {"new","\xf0\x9f\x86\x95","",Y,0},
    {"free","\xf0\x9f\x86\x93","",Y,0},
    {"up","\xf0\x9f\x86\x99","",Y,0},
    {"cool","\xf0\x9f\x86\x92","",Y,0},
    {"ng","\xf0\x9f\x86\x96","",Y,0},
    {"ok","\xf0\x9f\x86\x97","",Y,0},
    {"sos","\xf0\x9f\x86\x98","help emergency",Y,0},
    {"vs","\xf0\x9f\x86\x9a","versus",Y,0},
    {"id","\xf0\x9f\x86\x94","",Y,0},
    {"abc","\xf0\x9f\x94\xa4","",Y,0},
    {"abcd","\xf0\x9f\x94\xa1","",Y,0},
    {"capital_abcd","\xf0\x9f\x94\xa0","",Y,0},
    {"symbols","\xf0\x9f\x94\xa3","",Y,0},
    {"information_source","\xe2\x84\xb9\xef\xb8\x8f","info",Y,0},
    {"keycap_ten","\xf0\x9f\x94\x9f","10",Y,0},
    {"hash","\x23\xef\xb8\x8f\xe2\x83\xa3","number",Y,0},
    {"asterisk","\x2a\xef\xb8\x8f\xe2\x83\xa3","",Y,0},
    {"zero","\x30\xef\xb8\x8f\xe2\x83\xa3","",Y,0},
    {"one","\x31\xef\xb8\x8f\xe2\x83\xa3","",Y,0},
    {"two","\x32\xef\xb8\x8f\xe2\x83\xa3","",Y,0},
    {"three","\x33\xef\xb8\x8f\xe2\x83\xa3","",Y,0},
    {"four","\x34\xef\xb8\x8f\xe2\x83\xa3","",Y,0},
    {"five","\x35\xef\xb8\x8f\xe2\x83\xa3","",Y,0},
    {"six","\x36\xef\xb8\x8f\xe2\x83\xa3","",Y,0},
    {"seven","\x37\xef\xb8\x8f\xe2\x83\xa3","",Y,0},
    {"eight","\x38\xef\xb8\x8f\xe2\x83\xa3","",Y,0},
    {"nine","\x39\xef\xb8\x8f\xe2\x83\xa3","",Y,0},
    {"red_circle","\xf0\x9f\x94\xb4","",Y,0},
    {"orange_circle","\xf0\x9f\x9f\xa0","",Y,0},
    {"yellow_circle","\xf0\x9f\x9f\xa1","",Y,0},
    {"green_circle","\xf0\x9f\x9f\xa2","",Y,0},
    {"large_blue_circle","\xf0\x9f\x94\xb5","",Y,0},
    {"purple_circle","\xf0\x9f\x9f\xa3","",Y,0},
    {"brown_circle","\xf0\x9f\x9f\xa4","",Y,0},
    {"black_circle","\xe2\x9a\xab","",Y,0},
    {"white_circle","\xe2\x9a\xaa","",Y,0},
    {"red_square","\xf0\x9f\x9f\xa5","",Y,0},
    {"orange_square","\xf0\x9f\x9f\xa7","",Y,0},
    {"yellow_square","\xf0\x9f\x9f\xa8","",Y,0},
    {"green_square","\xf0\x9f\x9f\xa9","",Y,0},
    {"blue_square","\xf0\x9f\x9f\xa6","",Y,0},
    {"purple_square","\xf0\x9f\x9f\xaa","",Y,0},
    {"brown_square","\xf0\x9f\x9f\xab","",Y,0},
    {"black_large_square","\xe2\xac\x9b","",Y,0},
    {"white_large_square","\xe2\xac\x9c","",Y,0},
    {"small_red_triangle","\xf0\x9f\x94\xba","up",Y,0},
    {"small_red_triangle_down","\xf0\x9f\x94\xbb","down",Y,0},
    {"diamond_shape_with_a_dot_inside","\xf0\x9f\x92\xa0","",Y,0},
    {"radio_button","\xf0\x9f\x94\x98","",Y,0},
    {"eye","\xf0\x9f\x91\x81\xef\xb8\x8f","look watch",Y,0},
    {"eyes","\xf0\x9f\x91\x80","look watching",Y,0},
    {"wavy_dash","\xe3\x80\xb0\xef\xb8\x8f","",Y,0},
    {"curly_loop","\xe2\x9e\xb0","",Y,0},
    {"white_flower","\xf0\x9f\x92\xae","",Y,0},
    {"copyright","\xc2\xa9\xef\xb8\x8f","",Y,0},
    {"registered","\xc2\xae\xef\xb8\x8f","",Y,0},
    {"tm","\xe2\x84\xa2\xef\xb8\x8f","trademark",Y,0},
    {"checkered_flag","\xf0\x9f\x8f\x81","finish race",Y,0},
    {"triangular_flag_on_post","\xf0\x9f\x9a\xa9","flag",Y,0},
    {"crossed_flags","\xf0\x9f\x8e\x8c","",Y,0},
    {"black_flag","\xf0\x9f\x8f\xb4","",Y,0},
    {"white_flag","\xf0\x9f\x8f\xb3\xef\xb8\x8f","surrender",Y,0},
    {"rainbow_flag","\xf0\x9f\x8f\xb3\xef\xb8\x8f\xe2\x80\x8d\xf0\x9f\x8c\x88","pride lgbt",Y,0},
    {"pirate_flag","\xf0\x9f\x8f\xb4\xe2\x80\x8d\xe2\x98\xa0\xef\xb8\x8f","pirate",Y,0},
};

#undef S
#undef G
#undef P
#undef N
#undef F
#undef A
#undef O
#undef Y

/* If this fires, raise OC_EMOJI_MAX — every caller sizes its hit buffer by it. */
typedef char oc_emoji_fits[(sizeof EMOJI / sizeof *EMOJI) <= OC_EMOJI_MAX ? 1 : -1];

static const char *CAT_NAMES[OC_EMOJI_CAT_COUNT] = {
    "Smileys", "Gestures", "People", "Nature", "Food", "Activity", "Objects", "Symbols"
};

const oc_emoji *oc_emoji_all(size_t *count) {
    if (count) *count = sizeof EMOJI / sizeof *EMOJI;
    return EMOJI;
}

const char *oc_emoji_category_name(uint8_t category) {
    return category < OC_EMOJI_CAT_COUNT ? CAT_NAMES[category] : "";
}

/* Shortcodes that shipped before the catalogue was expanded and are no longer
 * the catalogue's own name for that emoji. They resolve, but are deliberately
 * NOT in EMOJI[]: a picker showing both `:nerd:` and `:nerd_face:` is showing
 * the same glyph twice. Messages already stored with the old spelling keep
 * rendering, which is the whole point — a shortcode that ever shipped can never
 * stop resolving. */
static const struct { const char *from; const char *to; } EMOJI_ALIASES[] = {
    {"slightly_smiling","slightly_smiling_face"},
    {"neutral","neutral_face"},
    {"zipper_mouth","zipper_mouth_face"},
    {"nerd","nerd_face"},
    {"hot","hot_face"},
    {"cold","cold_face"},
    {"woozy","woozy_face"},
    {"upside_down","upside_down_face"},
    {"melting","melting_face"},
    {"shushing","shushing_face"},
    {"salute","saluting_face"},
    {"party_face","partying_face"},
    {"hand","raised_hand"},
    {"person","adult"},
    {"sun","sunny"},
    {"wine","wine_glass"},
    {"confetti","confetti_ball"},
    {"medal","medal_sports"},
    {"chart_down","chart_with_downwards_trend"},
    {"inbox","inbox_tray"},
    {"outbox","outbox_tray"},
    {"phone","telephone_receiver"},
    {"floppy","floppy_disk"},
    {"alarm","alarm_clock"},
    {"money","moneybag"},
    {"check","white_check_mark"},
    {"heavy_check","heavy_check_mark"},
    {"eyes_symbol","eye"},
    /* The two Slack spells with punctuation. They are KEYWORDS of thumbsup and
     * thumbsdown, not names, so a lookup failed and every caller silently
     * dropped them: the default quick-reaction set is "+1,heart,joy,..." and
     * quietly became five reactions instead of six, with the first one missing
     * -- which is what a toast offering a heart where a thumb was meant looks
     * like. A reaction another client stored as ":+1:" also rendered as literal
     * text in the chip, for the same reason. */
    {"+1","thumbsup"},
    {"-1","thumbsdown"},
};

const char *oc_emoji_by_name(const char *name) {
    if (!name || !name[0]) return NULL;
    for (size_t i = 0; i < sizeof EMOJI / sizeof *EMOJI; i++)
        if (strcmp(EMOJI[i].name, name) == 0) return EMOJI[i].emoji;
    for (size_t i = 0; i < sizeof EMOJI_ALIASES / sizeof *EMOJI_ALIASES; i++)
        if (strcmp(EMOJI_ALIASES[i].from, name) == 0)
            return oc_emoji_by_name(EMOJI_ALIASES[i].to);
    return NULL;
}

/* U+1F3FB..U+1F3FF in UTF-8: the five Fitzpatrick modifiers, indexed by
 * OC_SKIN_LIGHT..OC_SKIN_DARK. */
static const char *const SKIN_MOD[] = {
    "", "\xf0\x9f\x8f\xbb", "\xf0\x9f\x8f\xbc", "\xf0\x9f\x8f\xbd",
    "\xf0\x9f\x8f\xbe", "\xf0\x9f\x8f\xbf"
};
static const char *const SKIN_NAMES[] = {
    "Default", "Light", "Medium-light", "Medium", "Medium-dark", "Dark"
};

const char *oc_emoji_skin_name(uint8_t tone) {
    return tone < OC_SKIN_COUNT ? SKIN_NAMES[tone] : "";
}

size_t oc_emoji_with_tone(const oc_emoji *e, uint8_t tone, char *out, size_t cap) {
    if (!e || !e->emoji || !out || cap == 0) return 0;
    size_t blen = strlen(e->emoji);
    if (!e->tonable || tone == OC_SKIN_DEFAULT || tone >= OC_SKIN_COUNT) {
        if (blen + 1 > cap) return 0;
        memcpy(out, e->emoji, blen + 1);
        return blen;
    }
    /* A variation selector (U+FE0F) asks for the emoji presentation of a
     * character that also has a text form. A skin-tone modifier already implies
     * it, and the pair is not a well-formed sequence, so the selector goes. */
    static const char VS16[] = "\xef\xb8\x8f";
    if (blen >= 3 && memcmp(e->emoji + blen - 3, VS16, 3) == 0) blen -= 3;
    const char *mod = SKIN_MOD[tone];
    size_t mlen = strlen(mod);
    if (blen + mlen + 1 > cap) return 0;
    memcpy(out, e->emoji, blen);
    memcpy(out + blen, mod, mlen);
    out[blen + mlen] = '\0';
    return blen + mlen;
}

/* Case-insensitive "does `s` start with `pre`". An empty prefix matches. */
static int ci_prefix(const char *s, const char *pre) {
    for (; *pre; s++, pre++)
        if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*pre)) return 0;
    return 1;
}

/* Case-insensitive "does any space-separated word of `hay` start with `pre`".
 * Word-prefix rather than substring so "art" finds :art: but not :heart:, which
 * is the ordering people expect from a shortcode search. */
static int ci_word_prefix(const char *hay, const char *pre) {
    if (!hay || !hay[0]) return 0;
    for (const char *p = hay; *p; ) {
        if (ci_prefix(p, pre)) return 1;
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
    }
    return 0;
}

size_t oc_emoji_search(const char *query, const oc_emoji **out, size_t max) {
    size_t n = 0, total = sizeof EMOJI / sizeof *EMOJI;
    for (size_t i = 0; i < total && n < max; i++) {
        if (!query || !query[0] ||
            ci_prefix(EMOJI[i].name, query) ||
            ci_word_prefix(EMOJI[i].keywords, query))
            out[n++] = &EMOJI[i];
    }
    return n;
}

/* ---- completion ----------------------------------------------------------- */

/* Substring, case-insensitively — the second band of oc_complete_targets. */
static int ci_contains(const char *s, const char *needle) {
    if (!s || !needle || !*needle) return 1;
    size_t nl = strlen(needle);
    for (const char *p = s; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}

size_t oc_complete_targets(const oc_model *m, const char *query,
                           oc_target *out, size_t max) {
    if (!m || !out || max == 0) return 0;
    const char *q = query ? query : "";
    /* A leading sigil is accepted and ignored rather than treated as a filter:
     * somebody typing "@ali" in a To: field means the person, not a literal. */
    if (*q == '@' || *q == '#') q++;
    size_t n = 0;
    /* Two passes so prefix matches lead, and within each pass people lead —
     * you address a person more often than a channel. */
    for (int band = 0; band < 2 && n < max; band++) {
        for (size_t i = 0; i < m->n_users && n < max; i++) {
            const char *nm = m->users[i].name;
            if (!nm || !nm[0] || m->users[i].user_id == m->user_id) continue;  /* not yourself */
            int pre = ci_prefix(nm, q);
            if (band == 0 ? !pre : (pre || !ci_contains(nm, q))) continue;
            out[n].id = m->users[i].user_id;
            out[n].is_channel = 0;
            snprintf(out[n].name, sizeof out[n].name, "%s", nm);
            /* The subtitle is their title if they set one — Slack shows the real
             * name beside the handle; ours has a title field and no second name. */
            snprintf(out[n].sub, sizeof out[n].sub, "%s", m->users[i].title);
            n++;
        }
        for (size_t i = 0; i < m->n_channels && n < max; i++) {
            const oc_channel *c = &m->channels[i];
            if (c->kind == OC_CHANNEL_KIND_DM || !c->name || !c->name[0]) continue;
            int pre = ci_prefix(c->name, q);
            if (band == 0 ? !pre : (pre || !ci_contains(c->name, q))) continue;
            out[n].id = c->channel_id;
            out[n].is_channel = 1;
            snprintf(out[n].name, sizeof out[n].name, "%s", c->name);
            out[n].sub[0] = '\0';
            n++;
        }
    }
    return n;
}

size_t oc_complete(const oc_model *m, const char *text,
                   oc_completion *out, size_t max, int *repl_start, int *kind) {
    if (kind) *kind = OC_AC_NONE;
    if (repl_start) *repl_start = 0;
    if (!m || !text || !out || max == 0) return 0;

    /* The trailing token: everything back to the last whitespace. */
    size_t len = strlen(text);
    int ws = 0;
    for (int i = (int)len - 1; i >= 0; i--)
        if (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r') { ws = i + 1; break; }
    const char *tok = text + ws;
    if (repl_start) *repl_start = ws;

    size_t n = 0;

    if (tok[0] == ':' && !strchr(tok + 1, ':')) {
        if (kind) *kind = OC_AC_EMOJI;
        const oc_emoji *hits[64];
        size_t nh = oc_emoji_search(tok + 1, hits, 64);
        for (size_t i = 0; i < nh && n < max; i++) {
            snprintf(out[n].repl, sizeof out[n].repl, "%s", hits[i]->emoji);
            snprintf(out[n].disp, sizeof out[n].disp, "%s  :%s:", hits[i]->emoji, hits[i]->name);
            n++;
        }
        return n;
    }

    if (tok[0] == '@') {
        if (kind) *kind = OC_AC_MENTION;
        for (size_t i = 0; i < m->n_users && n < max; i++)
            if (m->users[i].name[0] && ci_prefix(m->users[i].name, tok + 1)) {
                snprintf(out[n].repl, sizeof out[n].repl, "@%s", m->users[i].name);
                snprintf(out[n].disp, sizeof out[n].disp, "%s", m->users[i].name);
                n++;
            }
        return n;
    }

    if (tok[0] == '#') {
        if (kind) *kind = OC_AC_CHANNEL;
        for (size_t i = 0; i < m->n_channels && n < max; i++) {
            const oc_channel *c = &m->channels[i];
            if (c->kind != OC_CHANNEL_KIND_DM && c->name && ci_prefix(c->name, tok + 1)) {
                snprintf(out[n].repl, sizeof out[n].repl, "#%s", c->name);
                snprintf(out[n].disp, sizeof out[n].disp, "%s", c->name);
                n++;
            }
        }
        return n;
    }

    return 0;
}
