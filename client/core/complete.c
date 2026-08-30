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

static const oc_emoji EMOJI[] = {
    /* smileys */
    {"smile","\xf0\x9f\x98\x84","happy joy",S},
    {"grin","\xf0\x9f\x98\x81","happy",S},
    {"laughing","\xf0\x9f\x98\x86","lol haha",S},
    {"joy","\xf0\x9f\x98\x82","lol crying laugh",S},
    {"rofl","\xf0\x9f\xa4\xa3","lol rolling",S},
    {"slightly_smiling_face","\xf0\x9f\x99\x82","smile",S},
    {"upside_down_face","\xf0\x9f\x99\x83","silly",S},
    {"wink","\xf0\x9f\x98\x89","joke",S},
    {"blush","\xf0\x9f\x98\x8a","shy happy",S},
    {"innocent","\xf0\x9f\x98\x87","angel halo",S},
    {"smiling_face_with_three_hearts","\xf0\x9f\xa5\xb0","love adore",S},
    {"heart_eyes","\xf0\x9f\x98\x8d","love",S},
    {"star_struck","\xf0\x9f\xa4\xa9","amazed wow",S},
    {"kissing_heart","\xf0\x9f\x98\x98","kiss love",S},
    {"kissing","\xf0\x9f\x98\x97","kiss",S},
    {"kissing_closed_eyes","\xf0\x9f\x98\x9a","kiss",S},
    {"kissing_smiling_eyes","\xf0\x9f\x98\x99","kiss",S},
    {"smiling_face_with_tear","\xf0\x9f\xa5\xb2","grateful sad happy",S},
    {"sweat_smile","\xf0\x9f\x98\x85","relief phew nervous",S},
    {"melting_face","\xf0\x9f\xab\xa0","melting embarrassed",S},
    {"saluting_face","\xf0\x9f\xab\xa1","salute yes sir",S},
    {"yum","\xf0\x9f\x98\x8b","tasty delicious",S},
    {"stuck_out_tongue","\xf0\x9f\x98\x9b","tongue",S},
    {"stuck_out_tongue_winking_eye","\xf0\x9f\x98\x9c","tongue joke",S},
    {"zany_face","\xf0\x9f\xa4\xaa","crazy silly",S},
    {"stuck_out_tongue_closed_eyes","\xf0\x9f\x98\x9d","tongue",S},
    {"money_mouth_face","\xf0\x9f\xa4\x91","rich dollar",S},
    {"hugs","\xf0\x9f\xa4\x97","hug",S},
    {"hand_over_mouth","\xf0\x9f\xa4\xad","oops giggle",S},
    {"shushing_face","\xf0\x9f\xa4\xab","quiet secret",S},
    {"thinking","\xf0\x9f\xa4\x94","hmm consider",S},
    {"zipper_mouth_face","\xf0\x9f\xa4\x90","quiet secret",S},
    {"raised_eyebrow","\xf0\x9f\xa4\xa8","skeptical doubt",S},
    {"neutral_face","\xf0\x9f\x98\x90","meh",S},
    {"expressionless","\xf0\x9f\x98\x91","meh",S},
    {"no_mouth","\xf0\x9f\x98\xb6","silent",S},
    {"face_in_clouds","\xf0\x9f\x98\xb6\xe2\x80\x8d\xf0\x9f\x8c\xab\xef\xb8\x8f","confused foggy",S},
    {"smirk","\xf0\x9f\x98\x8f","smug",S},
    {"unamused","\xf0\x9f\x98\x92","meh annoyed",S},
    {"roll_eyes","\xf0\x9f\x99\x84","whatever",S},
    {"grimacing","\xf0\x9f\x98\xac","awkward yikes",S},
    {"exhale","\xf0\x9f\x98\xae\xe2\x80\x8d\xf0\x9f\x92\xa8","relief sigh",S},
    {"lying_face","\xf0\x9f\xa4\xa5","liar pinocchio",S},
    {"relieved","\xf0\x9f\x98\x8c","calm",S},
    {"pensive","\xf0\x9f\x98\x94","sad",S},
    {"sleepy","\xf0\x9f\x98\xaa","tired",S},
    {"drooling_face","\xf0\x9f\xa4\xa4","want",S},
    {"sleeping","\xf0\x9f\x98\xb4","zzz tired",S},
    {"mask","\xf0\x9f\x98\xb7","sick ill",S},
    {"face_with_thermometer","\xf0\x9f\xa4\x92","sick fever",S},
    {"face_with_head_bandage","\xf0\x9f\xa4\x95","hurt injured",S},
    {"nauseated_face","\xf0\x9f\xa4\xa2","sick gross",S},
    {"vomiting_face","\xf0\x9f\xa4\xae","sick gross",S},
    {"sneezing_face","\xf0\x9f\xa4\xa7","sick cold",S},
    {"hot_face","\xf0\x9f\xa5\xb5","heat sweating",S},
    {"cold_face","\xf0\x9f\xa5\xb6","freezing",S},
    {"woozy_face","\xf0\x9f\xa5\xb4","dizzy drunk",S},
    {"dizzy_face","\xf0\x9f\x98\xb5","stunned",S},
    {"exploding_head","\xf0\x9f\xa4\xaf","mind blown",S},
    {"cowboy_hat_face","\xf0\x9f\xa4\xa0","yeehaw",S},
    {"partying_face","\xf0\x9f\xa5\xb3","celebrate party",S},
    {"disguised_face","\xf0\x9f\xa5\xb8","incognito",S},
    {"sunglasses","\xf0\x9f\x98\x8e","cool",S},
    {"nerd_face","\xf0\x9f\xa4\x93","geek",S},
    {"monocle_face","\xf0\x9f\xa7\x90","inspect examine",S},
    {"confused","\xf0\x9f\x98\x95","unsure",S},
    {"worried","\xf0\x9f\x98\x9f","concerned",S},
    {"slightly_frowning_face","\xf0\x9f\x99\x81","sad",S},
    {"frowning_face","\xe2\x98\xb9\xef\xb8\x8f","sad",S},
    {"open_mouth","\xf0\x9f\x98\xae","surprised",S},
    {"hushed","\xf0\x9f\x98\xaf","surprised",S},
    {"astonished","\xf0\x9f\x98\xb2","shocked",S},
    {"flushed","\xf0\x9f\x98\xb3","embarrassed",S},
    {"pleading_face","\xf0\x9f\xa5\xba","please beg",S},
    {"frowning","\xf0\x9f\x98\xa6","sad",S},
    {"anguished","\xf0\x9f\x98\xa7","shocked",S},
    {"fearful","\xf0\x9f\x98\xa8","scared",S},
    {"cold_sweat","\xf0\x9f\x98\xb0","anxious nervous",S},
    {"disappointed_relieved","\xf0\x9f\x98\xa5","phew sad",S},
    {"cry","\xf0\x9f\x98\xa2","sad tear",S},
    {"sob","\xf0\x9f\x98\xad","crying bawling",S},
    {"scream","\xf0\x9f\x98\xb1","fear shock",S},
    {"confounded","\xf0\x9f\x98\x96","frustrated",S},
    {"persevere","\xf0\x9f\x98\xa3","struggle",S},
    {"disappointed","\xf0\x9f\x98\x9e","sad",S},
    {"sweat","\xf0\x9f\x98\x93","nervous",S},
    {"weary","\xf0\x9f\x98\xa9","tired exhausted",S},
    {"tired_face","\xf0\x9f\x98\xab","exhausted",S},
    {"yawning_face","\xf0\x9f\xa5\xb1","bored tired",S},
    {"triumph","\xf0\x9f\x98\xa4","proud steam",S},
    {"rage","\xf0\x9f\x98\xa1","angry mad",S},
    {"angry","\xf0\x9f\x98\xa0","mad",S},
    {"cursing_face","\xf0\x9f\xa4\xac","swearing",S},
    {"smiling_imp","\xf0\x9f\x98\x88","devil mischief",S},
    {"imp","\xf0\x9f\x91\xbf","devil angry",S},
    {"skull","\xf0\x9f\x92\x80","dead",S},
    {"skull_and_crossbones","\xe2\x98\xa0\xef\xb8\x8f","danger poison",S},
    {"poop","\xf0\x9f\x92\xa9","crap",S},
    {"clown_face","\xf0\x9f\xa4\xa1","clown",S},
    {"ghost","\xf0\x9f\x91\xbb","boo halloween",S},
    {"alien","\xf0\x9f\x91\xbd","ufo",S},
    {"space_invader","\xf0\x9f\x91\xbe","game alien",S},
    {"robot","\xf0\x9f\xa4\x96","bot",S},
    {"jack_o_lantern","\xf0\x9f\x8e\x83","halloween pumpkin",S},
    {"smiley_cat","\xf0\x9f\x98\xba","cat",S},
    {"smile_cat","\xf0\x9f\x98\xb8","cat",S},
    {"joy_cat","\xf0\x9f\x98\xb9","cat lol",S},
    {"heart_eyes_cat","\xf0\x9f\x98\xbb","cat love",S},
    {"smirk_cat","\xf0\x9f\x98\xbc","cat",S},
    {"kissing_cat","\xf0\x9f\x98\xbd","cat",S},
    {"scream_cat","\xf0\x9f\x99\x80","cat shock",S},
    {"crying_cat_face","\xf0\x9f\x98\xbf","cat sad",S},
    {"pouting_cat","\xf0\x9f\x98\xbe","cat angry",S},
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
    {"handshake","\xf0\x9f\xa4\x9d","deal agree",G},
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
    {"technologist","\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x92\xbb","developer engineer",P},
    {"mechanic","\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x94\xa7","repair",P},
    {"scientist","\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x94\xac","research lab",P},
    {"teacher","\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x8f\xab","school",P},
    {"judge","\xf0\x9f\xa7\x91\xe2\x80\x8d\xe2\x9a\x96\xef\xb8\x8f","law",P},
    {"farmer","\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x8c\xbe","agriculture",P},
    {"cook","\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x8d\xb3","chef",P},
    {"student","\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x8e\x93","graduate",P},
    {"singer","\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x8e\xa4","musician",P},
    {"artist","\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x8e\xa8","painter",P},
    {"pilot","\xf0\x9f\xa7\x91\xe2\x80\x8d\xe2\x9c\x88\xef\xb8\x8f","aviation",P},
    {"astronaut","\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x9a\x80","space",P},
    {"firefighter","\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x9a\x92","fire",P},
    {"police_officer","\xf0\x9f\x91\xae","cop",P,1},
    {"detective","\xf0\x9f\x95\xb5\xef\xb8\x8f","spy investigate",P,1},
    {"guard","\xf0\x9f\x92\x82","",P,1},
    {"construction_worker","\xf0\x9f\x91\xb7","builder",P,1},
    {"superhero","\xf0\x9f\xa6\xb8","hero",P,1},
    {"supervillain","\xf0\x9f\xa6\xb9","villain",P,1},
    {"mage","\xf0\x9f\xa7\x99","wizard",P,1},
    {"fairy","\xf0\x9f\xa7\x9a","",P,1},
    {"vampire","\xf0\x9f\xa7\x9b","",P,1},
    {"zombie","\xf0\x9f\xa7\x9f","undead",P},
    {"person_in_lotus_position","\xf0\x9f\xa7\x98","yoga meditate",P,1},
    {"massage","\xf0\x9f\x92\x86","spa",P,1},
    {"haircut","\xf0\x9f\x92\x87","salon",P,1},
    {"walking","\xf0\x9f\x9a\xb6","walk",P,1},
    {"running","\xf0\x9f\x8f\x83","run exercise",P,1},
    {"dancer","\xf0\x9f\x92\x83","dance",P,1},
    {"man_dancing","\xf0\x9f\x95\xba","dance",P,1},
    {"people_hugging","\xf0\x9f\xab\x82","hug support",P},
    {"family","\xf0\x9f\x91\xaa","",P},
    {"couple","\xf0\x9f\x91\xab","",P},
    {"bust_in_silhouette","\xf0\x9f\x91\xa4","user person",P},
    {"busts_in_silhouette","\xf0\x9f\x91\xa5","users people",P},
    {"speaking_head","\xf0\x9f\x97\xa3\xef\xb8\x8f","talk say",P},
    /* nature */
    {"dog","\xf0\x9f\x90\xb6","puppy",N},
    {"cat","\xf0\x9f\x90\xb1","kitten",N},
    {"mouse","\xf0\x9f\x90\xad","",N},
    {"hamster","\xf0\x9f\x90\xb9","",N},
    {"rabbit","\xf0\x9f\x90\xb0","bunny",N},
    {"fox_face","\xf0\x9f\xa6\x8a","fox",N},
    {"bear","\xf0\x9f\x90\xbb","",N},
    {"panda_face","\xf0\x9f\x90\xbc","panda",N},
    {"polar_bear","\xf0\x9f\x90\xbb\xe2\x80\x8d\xe2\x9d\x84\xef\xb8\x8f","",N},
    {"koala","\xf0\x9f\x90\xa8","",N},
    {"tiger","\xf0\x9f\x90\xaf","",N},
    {"lion","\xf0\x9f\xa6\x81","",N},
    {"cow","\xf0\x9f\x90\xae","",N},
    {"pig","\xf0\x9f\x90\xb7","",N},
    {"frog","\xf0\x9f\x90\xb8","",N},
    {"monkey_face","\xf0\x9f\x90\xb5","monkey",N},
    {"see_no_evil","\xf0\x9f\x99\x88","monkey",N},
    {"hear_no_evil","\xf0\x9f\x99\x89","monkey",N},
    {"speak_no_evil","\xf0\x9f\x99\x8a","monkey",N},
    {"chicken","\xf0\x9f\x90\x94","",N},
    {"penguin","\xf0\x9f\x90\xa7","",N},
    {"bird","\xf0\x9f\x90\xa6","",N},
    {"baby_chick","\xf0\x9f\x90\xa4","chick",N},
    {"eagle","\xf0\x9f\xa6\x85","",N},
    {"duck","\xf0\x9f\xa6\x86","",N},
    {"owl","\xf0\x9f\xa6\x89","",N},
    {"bat","\xf0\x9f\xa6\x87","",N},
    {"wolf","\xf0\x9f\x90\xba","",N},
    {"boar","\xf0\x9f\x90\x97","",N},
    {"horse","\xf0\x9f\x90\xb4","",N},
    {"unicorn","\xf0\x9f\xa6\x84","",N},
    {"bee","\xf0\x9f\x90\x9d","honeybee",N},
    {"bug","\xf0\x9f\x90\x9b","caterpillar",N},
    {"butterfly","\xf0\x9f\xa6\x8b","",N},
    {"snail","\xf0\x9f\x90\x8c","slow",N},
    {"lady_beetle","\xf0\x9f\x90\x9e","ladybug",N},
    {"ant","\xf0\x9f\x90\x9c","",N},
    {"spider","\xf0\x9f\x95\xb7\xef\xb8\x8f","",N},
    {"scorpion","\xf0\x9f\xa6\x82","",N},
    {"turtle","\xf0\x9f\x90\xa2","",N},
    {"snake","\xf0\x9f\x90\x8d","",N},
    {"lizard","\xf0\x9f\xa6\x8e","",N},
    {"t_rex","\xf0\x9f\xa6\x96","dinosaur",N},
    {"octopus","\xf0\x9f\x90\x99","",N},
    {"squid","\xf0\x9f\xa6\x91","",N},
    {"shrimp","\xf0\x9f\xa6\x90","",N},
    {"crab","\xf0\x9f\xa6\x80","",N},
    {"blowfish","\xf0\x9f\x90\xa1","",N},
    {"tropical_fish","\xf0\x9f\x90\xa0","",N},
    {"fish","\xf0\x9f\x90\x9f","",N},
    {"dolphin","\xf0\x9f\x90\xac","",N},
    {"whale","\xf0\x9f\x90\xb3","",N},
    {"shark","\xf0\x9f\xa6\x88","",N},
    {"crocodile","\xf0\x9f\x90\x8a","",N},
    {"leopard","\xf0\x9f\x90\x86","",N},
    {"zebra","\xf0\x9f\xa6\x93","",N},
    {"gorilla","\xf0\x9f\xa6\x8d","",N},
    {"elephant","\xf0\x9f\x90\x98","",N},
    {"hippopotamus","\xf0\x9f\xa6\x9b","",N},
    {"rhinoceros","\xf0\x9f\xa6\x8f","",N},
    {"camel","\xf0\x9f\x90\xab","",N},
    {"giraffe","\xf0\x9f\xa6\x92","",N},
    {"kangaroo","\xf0\x9f\xa6\x98","",N},
    {"sheep","\xf0\x9f\x90\x91","",N},
    {"goat","\xf0\x9f\x90\x90","",N},
    {"deer","\xf0\x9f\xa6\x8c","",N},
    {"hedgehog","\xf0\x9f\xa6\x94","",N},
    {"paw_prints","\xf0\x9f\x90\xbe","pets",N},
    {"dragon","\xf0\x9f\x90\x89","",N},
    {"cactus","\xf0\x9f\x8c\xb5","",N},
    {"christmas_tree","\xf0\x9f\x8e\x84","xmas",N},
    {"evergreen_tree","\xf0\x9f\x8c\xb2","tree",N},
    {"deciduous_tree","\xf0\x9f\x8c\xb3","tree",N},
    {"palm_tree","\xf0\x9f\x8c\xb4","",N},
    {"seedling","\xf0\x9f\x8c\xb1","plant sprout",N},
    {"herb","\xf0\x9f\x8c\xbf","leaf",N},
    {"shamrock","\xe2\x98\x98\xef\xb8\x8f","luck",N},
    {"four_leaf_clover","\xf0\x9f\x8d\x80","luck",N},
    {"maple_leaf","\xf0\x9f\x8d\x81","autumn",N},
    {"fallen_leaf","\xf0\x9f\x8d\x82","autumn",N},
    {"leaves","\xf0\x9f\x8d\x83","wind",N},
    {"mushroom","\xf0\x9f\x8d\x84","",N},
    {"bouquet","\xf0\x9f\x92\x90","flowers",N},
    {"tulip","\xf0\x9f\x8c\xb7","",N},
    {"rose","\xf0\x9f\x8c\xb9","",N},
    {"wilted_flower","\xf0\x9f\xa5\x80","dead",N},
    {"sunflower","\xf0\x9f\x8c\xbb","",N},
    {"blossom","\xf0\x9f\x8c\xbc","flower",N},
    {"cherry_blossom","\xf0\x9f\x8c\xb8","sakura",N},
    {"hibiscus","\xf0\x9f\x8c\xba","",N},
    {"earth_africa","\xf0\x9f\x8c\x8d","world globe",N},
    {"earth_americas","\xf0\x9f\x8c\x8e","world globe",N},
    {"earth_asia","\xf0\x9f\x8c\x8f","world globe",N},
    {"full_moon","\xf0\x9f\x8c\x95","",N},
    {"crescent_moon","\xf0\x9f\x8c\x99","night",N},
    {"star","\xe2\xad\x90","favorite",N},
    {"star2","\xf0\x9f\x8c\x9f","sparkle",N},
    {"sparkles","\xe2\x9c\xa8","magic shiny",N},
    {"zap","\xe2\x9a\xa1","lightning fast",N},
    {"fire","\xf0\x9f\x94\xa5","hot lit",N},
    {"boom","\xf0\x9f\x92\xa5","explosion",N},
    {"comet","\xe2\x98\x84\xef\xb8\x8f","",N},
    {"sunny","\xe2\x98\x80\xef\xb8\x8f","sun clear",N},
    {"partly_sunny","\xe2\x9b\x85","cloud",N},
    {"cloud","\xe2\x98\x81\xef\xb8\x8f","",N},
    {"rainbow","\xf0\x9f\x8c\x88","",N},
    {"umbrella","\xe2\x98\x94","rain",N},
    {"snowflake","\xe2\x9d\x84\xef\xb8\x8f","snow cold",N},
    {"snowman","\xe2\x9b\x84","",N},
    {"wind_face","\xf0\x9f\x8c\xac\xef\xb8\x8f","wind",N},
    {"tornado","\xf0\x9f\x8c\xaa\xef\xb8\x8f","",N},
    {"ocean","\xf0\x9f\x8c\x8a","wave water",N},
    {"droplet","\xf0\x9f\x92\xa7","water",N},
    {"volcano","\xf0\x9f\x8c\x8b","",N},
    {"mountain","\xe2\x9b\xb0\xef\xb8\x8f","",N},
    {"desert","\xf0\x9f\x8f\x9c\xef\xb8\x8f","",N},
    {"beach_umbrella","\xf0\x9f\x8f\x96\xef\xb8\x8f","beach",N},
    /* food */
    {"green_apple","\xf0\x9f\x8d\x8f","",F},
    {"apple","\xf0\x9f\x8d\x8e","",F},
    {"pear","\xf0\x9f\x8d\x90","",F},
    {"tangerine","\xf0\x9f\x8d\x8a","orange",F},
    {"lemon","\xf0\x9f\x8d\x8b","",F},
    {"banana","\xf0\x9f\x8d\x8c","",F},
    {"watermelon","\xf0\x9f\x8d\x89","",F},
    {"grapes","\xf0\x9f\x8d\x87","",F},
    {"strawberry","\xf0\x9f\x8d\x93","",F},
    {"blueberries","\xf0\x9f\xab\x90","",F},
    {"melon","\xf0\x9f\x8d\x88","",F},
    {"cherries","\xf0\x9f\x8d\x92","",F},
    {"peach","\xf0\x9f\x8d\x91","",F},
    {"mango","\xf0\x9f\xa5\xad","",F},
    {"pineapple","\xf0\x9f\x8d\x8d","",F},
    {"coconut","\xf0\x9f\xa5\xa5","",F},
    {"kiwi_fruit","\xf0\x9f\xa5\x9d","kiwi",F},
    {"tomato","\xf0\x9f\x8d\x85","",F},
    {"eggplant","\xf0\x9f\x8d\x86","",F},
    {"avocado","\xf0\x9f\xa5\x91","",F},
    {"broccoli","\xf0\x9f\xa5\xa6","",F},
    {"leafy_green","\xf0\x9f\xa5\xac","lettuce",F},
    {"cucumber","\xf0\x9f\xa5\x92","",F},
    {"hot_pepper","\xf0\x9f\x8c\xb6\xef\xb8\x8f","spicy",F},
    {"corn","\xf0\x9f\x8c\xbd","",F},
    {"carrot","\xf0\x9f\xa5\x95","",F},
    {"garlic","\xf0\x9f\xa7\x84","",F},
    {"onion","\xf0\x9f\xa7\x85","",F},
    {"potato","\xf0\x9f\xa5\x94","",F},
    {"sweet_potato","\xf0\x9f\x8d\xa0","",F},
    {"croissant","\xf0\x9f\xa5\x90","",F},
    {"bagel","\xf0\x9f\xa5\xaf","",F},
    {"bread","\xf0\x9f\x8d\x9e","",F},
    {"baguette_bread","\xf0\x9f\xa5\x96","baguette",F},
    {"pretzel","\xf0\x9f\xa5\xa8","",F},
    {"cheese","\xf0\x9f\xa7\x80","",F},
    {"egg","\xf0\x9f\xa5\x9a","",F},
    {"cooking","\xf0\x9f\x8d\xb3","fried egg",F},
    {"butter","\xf0\x9f\xa7\x88","",F},
    {"pancakes","\xf0\x9f\xa5\x9e","",F},
    {"waffle","\xf0\x9f\xa7\x87","",F},
    {"bacon","\xf0\x9f\xa5\x93","",F},
    {"cut_of_meat","\xf0\x9f\xa5\xa9","steak",F},
    {"poultry_leg","\xf0\x9f\x8d\x97","chicken",F},
    {"meat_on_bone","\xf0\x9f\x8d\x96","",F},
    {"hotdog","\xf0\x9f\x8c\xad","hot dog",F},
    {"hamburger","\xf0\x9f\x8d\x94","burger",F},
    {"fries","\xf0\x9f\x8d\x9f","chips",F},
    {"pizza","\xf0\x9f\x8d\x95","",F},
    {"sandwich","\xf0\x9f\xa5\xaa","",F},
    {"taco","\xf0\x9f\x8c\xae","",F},
    {"burrito","\xf0\x9f\x8c\xaf","",F},
    {"stuffed_flatbread","\xf0\x9f\xa5\x99","kebab",F},
    {"falafel","\xf0\x9f\xa7\x86","",F},
    {"salad","\xf0\x9f\xa5\x97","",F},
    {"shallow_pan_of_food","\xf0\x9f\xa5\x98","paella",F},
    {"stew","\xf0\x9f\x8d\xb2","",F},
    {"bowl_with_spoon","\xf0\x9f\xa5\xa3","cereal",F},
    {"canned_food","\xf0\x9f\xa5\xab","",F},
    {"spaghetti","\xf0\x9f\x8d\x9d","pasta",F},
    {"ramen","\xf0\x9f\x8d\x9c","noodles",F},
    {"curry","\xf0\x9f\x8d\x9b","",F},
    {"sushi","\xf0\x9f\x8d\xa3","",F},
    {"bento","\xf0\x9f\x8d\xb1","",F},
    {"dumpling","\xf0\x9f\xa5\x9f","",F},
    {"fortune_cookie","\xf0\x9f\xa5\xa0","",F},
    {"rice","\xf0\x9f\x8d\x9a","",F},
    {"rice_ball","\xf0\x9f\x8d\x99","",F},
    {"oden","\xf0\x9f\x8d\xa2","",F},
    {"fish_cake","\xf0\x9f\x8d\xa5","",F},
    {"shaved_ice","\xf0\x9f\x8d\xa7","",F},
    {"ice_cream","\xf0\x9f\x8d\xa8","",F},
    {"icecream","\xf0\x9f\x8d\xa6","soft serve",F},
    {"doughnut","\xf0\x9f\x8d\xa9","donut",F},
    {"cookie","\xf0\x9f\x8d\xaa","",F},
    {"popcorn","\xf0\x9f\x8d\xbf","movie snack",F},
    {"birthday","\xf0\x9f\x8e\x82","cake celebrate",F},
    {"cake","\xf0\x9f\x8d\xb0","dessert",F},
    {"cupcake","\xf0\x9f\xa7\x81","",F},
    {"pie","\xf0\x9f\xa5\xa7","",F},
    {"chocolate_bar","\xf0\x9f\x8d\xab","chocolate",F},
    {"candy","\xf0\x9f\x8d\xac","",F},
    {"lollipop","\xf0\x9f\x8d\xad","",F},
    {"honey_pot","\xf0\x9f\x8d\xaf","honey",F},
    {"baby_bottle","\xf0\x9f\x8d\xbc","",F},
    {"milk_glass","\xf0\x9f\xa5\x9b","milk",F},
    {"coffee","\xe2\x98\x95","tea espresso",F},
    {"teapot","\xf0\x9f\xab\x96","",F},
    {"tea","\xf0\x9f\x8d\xb5","green tea",F},
    {"sake","\xf0\x9f\x8d\xb6","",F},
    {"champagne","\xf0\x9f\x8d\xbe","celebrate",F},
    {"wine_glass","\xf0\x9f\x8d\xb7","wine",F},
    {"cocktail","\xf0\x9f\x8d\xb8","martini",F},
    {"tropical_drink","\xf0\x9f\x8d\xb9","",F},
    {"beer","\xf0\x9f\x8d\xba","",F},
    {"beers","\xf0\x9f\x8d\xbb","cheers",F},
    {"clinking_glasses","\xf0\x9f\xa5\x82","cheers toast",F},
    {"tumbler_glass","\xf0\x9f\xa5\x83","whisky",F},
    {"cup_with_straw","\xf0\x9f\xa5\xa4","soda",F},
    {"bubble_tea","\xf0\x9f\xa7\x8b","boba",F},
    {"mate","\xf0\x9f\xa7\x89","",F},
    {"ice_cube","\xf0\x9f\xa7\x8a","ice",F},
    {"chopsticks","\xf0\x9f\xa5\xa2","",F},
    {"fork_and_knife","\xf0\x9f\x8d\xb4","eat",F},
    {"plate_with_cutlery","\xf0\x9f\x8d\xbd\xef\xb8\x8f","dinner",F},
    {"spoon","\xf0\x9f\xa5\x84","",F},
    {"salt","\xf0\x9f\xa7\x82","",F},
    /* activity */
    {"soccer","\xe2\x9a\xbd","football",A},
    {"basketball","\xf0\x9f\x8f\x80","",A},
    {"football","\xf0\x9f\x8f\x88","american football",A},
    {"baseball","\xe2\x9a\xbe","",A},
    {"softball","\xf0\x9f\xa5\x8e","",A},
    {"tennis","\xf0\x9f\x8e\xbe","",A},
    {"volleyball","\xf0\x9f\x8f\x90","",A},
    {"rugby_football","\xf0\x9f\x8f\x89","rugby",A},
    {"flying_disc","\xf0\x9f\xa5\x8f","frisbee",A},
    {"8ball","\xf0\x9f\x8e\xb1","pool billiards",A},
    {"bowling","\xf0\x9f\x8e\xb3","",A},
    {"cricket_game","\xf0\x9f\x8f\x8f","cricket",A},
    {"field_hockey","\xf0\x9f\x8f\x91","",A},
    {"ice_hockey","\xf0\x9f\x8f\x92","",A},
    {"lacrosse","\xf0\x9f\xa5\x8d","",A},
    {"ping_pong","\xf0\x9f\x8f\x93","table tennis",A},
    {"badminton","\xf0\x9f\x8f\xb8","",A},
    {"boxing_glove","\xf0\x9f\xa5\x8a","boxing",A},
    {"martial_arts_uniform","\xf0\x9f\xa5\x8b","karate judo",A},
    {"goal_net","\xf0\x9f\xa5\x85","goal",A},
    {"golf","\xe2\x9b\xb3","golfing",A},
    {"ice_skate","\xe2\x9b\xb8\xef\xb8\x8f","skating",A},
    {"fishing_pole_and_fish","\xf0\x9f\x8e\xa3","fishing",A},
    {"diving_mask","\xf0\x9f\xa4\xbf","diving",A},
    {"running_shirt_with_sash","\xf0\x9f\x8e\xbd","marathon",A},
    {"ski","\xf0\x9f\x8e\xbf","skiing",A},
    {"sled","\xf0\x9f\x9b\xb7","",A},
    {"curling_stone","\xf0\x9f\xa5\x8c","curling",A},
    {"dart","\xf0\x9f\x8e\xaf","target bullseye",A},
    {"yo_yo","\xf0\x9f\xaa\x80","",A},
    {"kite","\xf0\x9f\xaa\x81","",A},
    {"crystal_ball","\xf0\x9f\x94\xae","fortune magic",A},
    {"video_game","\xf0\x9f\x8e\xae","gaming",A},
    {"joystick","\xf0\x9f\x95\xb9\xef\xb8\x8f","",A},
    {"slot_machine","\xf0\x9f\x8e\xb0","gambling",A},
    {"game_die","\xf0\x9f\x8e\xb2","dice random",A},
    {"jigsaw","\xf0\x9f\xa7\xa9","puzzle",A},
    {"teddy_bear","\xf0\x9f\xa7\xb8","",A},
    {"chess_pawn","\xe2\x99\x9f\xef\xb8\x8f","chess",A},
    {"performing_arts","\xf0\x9f\x8e\xad","theater drama",A},
    {"art","\xf0\x9f\x8e\xa8","paint palette",A},
    {"thread","\xf0\x9f\xa7\xb5","sewing",A},
    {"yarn","\xf0\x9f\xa7\xb6","knitting",A},
    {"microphone","\xf0\x9f\x8e\xa4","sing karaoke",A},
    {"headphones","\xf0\x9f\x8e\xa7","music listen",A},
    {"musical_score","\xf0\x9f\x8e\xbc","music",A},
    {"musical_note","\xf0\x9f\x8e\xb5","music",A},
    {"notes","\xf0\x9f\x8e\xb6","music",A},
    {"saxophone","\xf0\x9f\x8e\xb7","",A},
    {"guitar","\xf0\x9f\x8e\xb8","",A},
    {"musical_keyboard","\xf0\x9f\x8e\xb9","piano",A},
    {"trumpet","\xf0\x9f\x8e\xba","",A},
    {"violin","\xf0\x9f\x8e\xbb","",A},
    {"drum","\xf0\x9f\xa5\x81","",A},
    {"banjo","\xf0\x9f\xaa\x95","",A},
    {"clapper","\xf0\x9f\x8e\xac","movie action",A},
    {"bow_and_arrow","\xf0\x9f\x8f\xb9","archery",A},
    {"trophy","\xf0\x9f\x8f\x86","win champion",A},
    {"first_place_medal","\xf0\x9f\xa5\x87","gold win",A},
    {"second_place_medal","\xf0\x9f\xa5\x88","silver",A},
    {"third_place_medal","\xf0\x9f\xa5\x89","bronze",A},
    {"medal_sports","\xf0\x9f\x8f\x85","medal",A},
    {"medal_military","\xf0\x9f\x8e\x96\xef\xb8\x8f","",A},
    {"ticket","\xf0\x9f\x8e\xab","",A},
    {"tickets","\xf0\x9f\x8e\x9f\xef\xb8\x8f","",A},
    {"circus_tent","\xf0\x9f\x8e\xaa","",A},
    {"carousel_horse","\xf0\x9f\x8e\xa0","",A},
    {"ferris_wheel","\xf0\x9f\x8e\xa1","",A},
    {"roller_coaster","\xf0\x9f\x8e\xa2","",A},
    {"balloon","\xf0\x9f\x8e\x88","party",A},
    {"tada","\xf0\x9f\x8e\x89","party celebrate hooray",A},
    {"confetti_ball","\xf0\x9f\x8e\x8a","party",A},
    {"gift","\xf0\x9f\x8e\x81","present",A},
    {"ribbon","\xf0\x9f\x8e\x80","",A},
    {"sparkler","\xf0\x9f\x8e\x87","",A},
    {"fireworks","\xf0\x9f\x8e\x86","",A},
    {"red_envelope","\xf0\x9f\xa7\xa7","",A},
    /* objects */
    {"watch","\xe2\x8c\x9a","time",O},
    {"iphone","\xf0\x9f\x93\xb1","phone mobile",O},
    {"computer","\xf0\x9f\x92\xbb","laptop",O},
    {"brain","\xf0\x9f\xa7\xa0","think smart",O},
    {"pushpin","\xf0\x9f\x93\x8c","pin",O},
    {"desktop_computer","\xf0\x9f\x96\xa5\xef\xb8\x8f","monitor",O},
    {"keyboard","\xe2\x8c\xa8\xef\xb8\x8f","",O},
    {"printer","\xf0\x9f\x96\xa8\xef\xb8\x8f","",O},
    {"computer_mouse","\xf0\x9f\x96\xb1\xef\xb8\x8f","mouse",O},
    {"floppy_disk","\xf0\x9f\x92\xbe","save",O},
    {"cd","\xf0\x9f\x92\xbf","disc",O},
    {"dvd","\xf0\x9f\x93\x80","",O},
    {"minidisc","\xf0\x9f\x92\xbd","",O},
    {"vhs","\xf0\x9f\x93\xbc","tape",O},
    {"camera","\xf0\x9f\x93\xb7","photo",O},
    {"camera_flash","\xf0\x9f\x93\xb8","photo",O},
    {"video_camera","\xf0\x9f\x93\xb9","",O},
    {"movie_camera","\xf0\x9f\x8e\xa5","film",O},
    {"film_projector","\xf0\x9f\x93\xbd\xef\xb8\x8f","",O},
    {"telephone_receiver","\xf0\x9f\x93\x9e","call",O},
    {"pager","\xf0\x9f\x93\x9f","",O},
    {"fax","\xf0\x9f\x93\xa0","",O},
    {"tv","\xf0\x9f\x93\xba","television",O},
    {"radio","\xf0\x9f\x93\xbb","",O},
    {"studio_microphone","\xf0\x9f\x8e\x99\xef\xb8\x8f","podcast",O},
    {"level_slider","\xf0\x9f\x8e\x9a\xef\xb8\x8f","",O},
    {"control_knobs","\xf0\x9f\x8e\x9b\xef\xb8\x8f","",O},
    {"compass","\xf0\x9f\xa7\xad","direction",O},
    {"stopwatch","\xe2\x8f\xb1\xef\xb8\x8f","timer",O},
    {"timer_clock","\xe2\x8f\xb2\xef\xb8\x8f","timer",O},
    {"alarm_clock","\xe2\x8f\xb0","alarm",O},
    {"hourglass","\xe2\x8c\x9b","time waiting",O},
    {"hourglass_flowing_sand","\xe2\x8f\xb3","time waiting",O},
    {"satellite","\xf0\x9f\x9b\xb0\xef\xb8\x8f","space orbit",O},
    {"battery","\xf0\x9f\x94\x8b","power",O},
    {"electric_plug","\xf0\x9f\x94\x8c","power",O},
    {"bulb","\xf0\x9f\x92\xa1","idea light",O},
    {"flashlight","\xf0\x9f\x94\xa6","torch",O},
    {"candle","\xf0\x9f\x95\xaf\xef\xb8\x8f","",O},
    {"wastebasket","\xf0\x9f\x97\x91\xef\xb8\x8f","trash delete bin",O},
    {"oil_drum","\xf0\x9f\x9b\xa2\xef\xb8\x8f","",O},
    {"money_with_wings","\xf0\x9f\x92\xb8","spend",O},
    {"dollar","\xf0\x9f\x92\xb5","money cash",O},
    {"yen","\xf0\x9f\x92\xb4","money",O},
    {"euro","\xf0\x9f\x92\xb6","money",O},
    {"pound","\xf0\x9f\x92\xb7","money",O},
    {"moneybag","\xf0\x9f\x92\xb0","money rich",O},
    {"credit_card","\xf0\x9f\x92\xb3","payment",O},
    {"gem","\xf0\x9f\x92\x8e","diamond jewel",O},
    {"balance_scale","\xe2\x9a\x96\xef\xb8\x8f","justice law",O},
    {"ladder","\xf0\x9f\xaa\x9c","",O},
    {"toolbox","\xf0\x9f\xa7\xb0","tools",O},
    {"wrench","\xf0\x9f\x94\xa7","fix tool",O},
    {"hammer","\xf0\x9f\x94\xa8","build tool",O},
    {"hammer_and_wrench","\xf0\x9f\x9b\xa0\xef\xb8\x8f","tools build",O},
    {"screwdriver","\xf0\x9f\xaa\x9b","",O},
    {"nut_and_bolt","\xf0\x9f\x94\xa9","",O},
    {"gear","\xe2\x9a\x99\xef\xb8\x8f","settings config",O},
    {"clamp","\xf0\x9f\x97\x9c\xef\xb8\x8f","",O},
    {"link","\xf0\x9f\x94\x97","url",O},
    {"chains","\xe2\x9b\x93\xef\xb8\x8f","",O},
    {"hook","\xf0\x9f\xaa\x9d","",O},
    {"magnet","\xf0\x9f\xa7\xb2","",O},
    {"test_tube","\xf0\x9f\xa7\xaa","science experiment",O},
    {"petri_dish","\xf0\x9f\xa7\xab","science",O},
    {"dna","\xf0\x9f\xa7\xac","genetics",O},
    {"microscope","\xf0\x9f\x94\xac","science",O},
    {"telescope","\xf0\x9f\x94\xad","space",O},
    {"satellite_antenna","\xf0\x9f\x93\xa1","signal",O},
    {"syringe","\xf0\x9f\x92\x89","vaccine shot",O},
    {"pill","\xf0\x9f\x92\x8a","medicine",O},
    {"stethoscope","\xf0\x9f\xa9\xba","doctor health",O},
    {"bandage","\xf0\x9f\xa9\xb9","",O},
    {"door","\xf0\x9f\x9a\xaa","",O},
    {"bed","\xf0\x9f\x9b\x8f\xef\xb8\x8f","sleep",O},
    {"couch_and_lamp","\xf0\x9f\x9b\x8b\xef\xb8\x8f","sofa",O},
    {"chair","\xf0\x9f\xaa\x91","",O},
    {"toilet","\xf0\x9f\x9a\xbd","",O},
    {"shower","\xf0\x9f\x9a\xbf","",O},
    {"bathtub","\xf0\x9f\x9b\x81","",O},
    {"soap","\xf0\x9f\xa7\xbc","wash",O},
    {"sponge","\xf0\x9f\xa7\xbd","",O},
    {"broom","\xf0\x9f\xa7\xb9","clean sweep",O},
    {"basket","\xf0\x9f\xa7\xba","",O},
    {"roll_of_paper","\xf0\x9f\xa7\xbb","toilet paper",O},
    {"bucket","\xf0\x9f\xaa\xa3","",O},
    {"key","\xf0\x9f\x94\x91","password unlock",O},
    {"lock","\xf0\x9f\x94\x92","secure private",O},
    {"unlock","\xf0\x9f\x94\x93","open",O},
    {"closed_lock_with_key","\xf0\x9f\x94\x90","secure",O},
    {"shield","\xf0\x9f\x9b\xa1\xef\xb8\x8f","security protect",O},
    {"package","\xf0\x9f\x93\xa6","box shipping",O},
    {"mailbox","\xf0\x9f\x93\xab","mail",O},
    {"envelope","\xe2\x9c\x89\xef\xb8\x8f","mail email",O},
    {"email","\xf0\x9f\x93\xa7","mail",O},
    {"incoming_envelope","\xf0\x9f\x93\xa8","mail",O},
    {"outbox_tray","\xf0\x9f\x93\xa4","send",O},
    {"inbox_tray","\xf0\x9f\x93\xa5","receive",O},
    {"postbox","\xf0\x9f\x93\xae","mail",O},
    {"memo","\xf0\x9f\x93\x9d","note write edit",O},
    {"page_facing_up","\xf0\x9f\x93\x84","document file",O},
    {"page_with_curl","\xf0\x9f\x93\x83","document",O},
    {"bookmark_tabs","\xf0\x9f\x93\x91","",O},
    {"scroll","\xf0\x9f\x93\x9c","document",O},
    {"clipboard","\xf0\x9f\x93\x8b","copy list",O},
    {"calendar","\xf0\x9f\x93\x85","date schedule",O},
    {"spiral_calendar","\xf0\x9f\x97\x93\xef\xb8\x8f","schedule",O},
    {"date","\xf0\x9f\x93\x86","calendar",O},
    {"card_index_dividers","\xf0\x9f\x97\x82\xef\xb8\x8f","files folders",O},
    {"file_folder","\xf0\x9f\x93\x81","folder directory",O},
    {"open_file_folder","\xf0\x9f\x93\x82","folder",O},
    {"card_file_box","\xf0\x9f\x97\x83\xef\xb8\x8f","archive",O},
    {"file_cabinet","\xf0\x9f\x97\x84\xef\xb8\x8f","storage",O},
    {"newspaper","\xf0\x9f\x93\xb0","news",O},
    {"book","\xf0\x9f\x93\x96","read",O},
    {"books","\xf0\x9f\x93\x9a","library reading",O},
    {"notebook","\xf0\x9f\x93\x93","",O},
    {"ledger","\xf0\x9f\x93\x92","",O},
    {"closed_book","\xf0\x9f\x93\x95","",O},
    {"green_book","\xf0\x9f\x93\x97","",O},
    {"blue_book","\xf0\x9f\x93\x98","",O},
    {"orange_book","\xf0\x9f\x93\x99","",O},
    {"bookmark","\xf0\x9f\x94\x96","save",O},
    {"label","\xf0\x9f\x8f\xb7\xef\xb8\x8f","tag",O},
    {"paperclip","\xf0\x9f\x93\x8e","attachment",O},
    {"paperclips","\xf0\x9f\x96\x87\xef\xb8\x8f","attachments",O},
    {"straight_ruler","\xf0\x9f\x93\x8f","measure",O},
    {"triangular_ruler","\xf0\x9f\x93\x90","measure",O},
    {"scissors","\xe2\x9c\x82\xef\xb8\x8f","cut",O},
    {"pen","\xf0\x9f\x96\x8a\xef\xb8\x8f","write",O},
    {"fountain_pen","\xf0\x9f\x96\x8b\xef\xb8\x8f","write",O},
    {"pencil2","\xe2\x9c\x8f\xef\xb8\x8f","write edit",O},
    {"crayon","\xf0\x9f\x96\x8d\xef\xb8\x8f","",O},
    {"paintbrush","\xf0\x9f\x96\x8c\xef\xb8\x8f","paint",O},
    {"mag","\xf0\x9f\x94\x8d","search find zoom",O},
    {"mag_right","\xf0\x9f\x94\x8e","search",O},
    {"bar_chart","\xf0\x9f\x93\x8a","stats analytics",O},
    {"chart_with_upwards_trend","\xf0\x9f\x93\x88","growth up",O},
    {"chart_with_downwards_trend","\xf0\x9f\x93\x89","decline down",O},
    {"chart","\xf0\x9f\x92\xb9","money chart",O},
    {"abacus","\xf0\x9f\xa7\xae","math count",O},
    {"bell","\xf0\x9f\x94\x94","notification alert",O},
    {"no_bell","\xf0\x9f\x94\x95","mute silence",O},
    {"loudspeaker","\xf0\x9f\x93\xa2","announce",O},
    {"mega","\xf0\x9f\x93\xa3","announce shout",O},
    {"postal_horn","\xf0\x9f\x93\xaf","",O},
    {"cinema","\xf0\x9f\x8e\xa6","movie",O},
    {"bomb","\xf0\x9f\x92\xa3","explosive",O},
    {"hole","\xf0\x9f\x95\xb3\xef\xb8\x8f","",O},
    {"thermometer","\xf0\x9f\x8c\xa1\xef\xb8\x8f","temperature",O},
    {"world_map","\xf0\x9f\x97\xba\xef\xb8\x8f","map",O},
    {"rocket","\xf0\x9f\x9a\x80","launch ship fast",O},
    {"airplane","\xe2\x9c\x88\xef\xb8\x8f","flight travel",O},
    {"helicopter","\xf0\x9f\x9a\x81","",O},
    {"car","\xf0\x9f\x9a\x97","auto",O},
    {"taxi","\xf0\x9f\x9a\x95","",O},
    {"bus","\xf0\x9f\x9a\x8c","",O},
    {"truck","\xf0\x9f\x9a\x9a","delivery",O},
    {"bike","\xf0\x9f\x9a\xb2","bicycle",O},
    {"scooter","\xf0\x9f\x9b\xb4","",O},
    {"motorcycle","\xf0\x9f\x8f\x8d\xef\xb8\x8f","",O},
    {"train","\xf0\x9f\x9a\x86","rail",O},
    {"metro","\xf0\x9f\x9a\x87","subway",O},
    {"tram","\xf0\x9f\x9a\x8a","",O},
    {"ship","\xf0\x9f\x9a\xa2","boat",O},
    {"sailboat","\xe2\x9b\xb5","boat",O},
    {"speedboat","\xf0\x9f\x9a\xa4","boat",O},
    {"anchor","\xe2\x9a\x93","",O},
    {"construction","\xf0\x9f\x9a\xa7","wip roadwork",O},
    {"traffic_light","\xf0\x9f\x9a\xa6","",O},
    {"house","\xf0\x9f\x8f\xa0","home",O},
    {"office","\xf0\x9f\x8f\xa2","building work",O},
    {"hospital","\xf0\x9f\x8f\xa5","",O},
    {"bank","\xf0\x9f\x8f\xa6","",O},
    {"school","\xf0\x9f\x8f\xab","",O},
    {"factory","\xf0\x9f\x8f\xad","",O},
    {"hotel","\xf0\x9f\x8f\xa8","",O},
    {"church","\xe2\x9b\xaa","",O},
    {"stadium","\xf0\x9f\x8f\x9f\xef\xb8\x8f","",O},
    {"classical_building","\xf0\x9f\x8f\x9b\xef\xb8\x8f","museum",O},
    {"statue_of_liberty","\xf0\x9f\x97\xbd","",O},
    {"bridge_at_night","\xf0\x9f\x8c\x89","bridge",O},
    {"city_sunset","\xf0\x9f\x8c\x87","city",O},
    {"night_with_stars","\xf0\x9f\x8c\x83","city night",O},
    {"cityscape","\xf0\x9f\x8f\x99\xef\xb8\x8f","city skyline",O},
    {"tent","\xe2\x9b\xba","camping",O},
    {"national_park","\xf0\x9f\x8f\x9e\xef\xb8\x8f","nature",O},
    {"mount_fuji","\xf0\x9f\x97\xbb","",O},
    /* symbols */
    {"heart","\xe2\x9d\xa4\xef\xb8\x8f","love",Y},
    {"orange_heart","\xf0\x9f\xa7\xa1","love",Y},
    {"yellow_heart","\xf0\x9f\x92\x9b","love",Y},
    {"green_heart","\xf0\x9f\x92\x9a","love",Y},
    {"blue_heart","\xf0\x9f\x92\x99","love",Y},
    {"purple_heart","\xf0\x9f\x92\x9c","love",Y},
    {"black_heart","\xf0\x9f\x96\xa4","love",Y},
    {"white_heart","\xf0\x9f\xa4\x8d","love",Y},
    {"brown_heart","\xf0\x9f\xa4\x8e","love",Y},
    {"broken_heart","\xf0\x9f\x92\x94","sad heartbreak",Y},
    {"heart_on_fire","\xe2\x9d\xa4\xef\xb8\x8f\xe2\x80\x8d\xf0\x9f\x94\xa5","passion",Y},
    {"mending_heart","\xe2\x9d\xa4\xef\xb8\x8f\xe2\x80\x8d\xf0\x9f\xa9\xb9","healing",Y},
    {"two_hearts","\xf0\x9f\x92\x95","love",Y},
    {"sparkling_heart","\xf0\x9f\x92\x96","love",Y},
    {"heartpulse","\xf0\x9f\x92\x97","love",Y},
    {"heartbeat","\xf0\x9f\x92\x93","love",Y},
    {"revolving_hearts","\xf0\x9f\x92\x9e","love",Y},
    {"cupid","\xf0\x9f\x92\x98","love arrow",Y},
    {"gift_heart","\xf0\x9f\x92\x9d","love",Y},
    {"100","\xf0\x9f\x92\xaf","perfect score hundred",Y},
    {"anger","\xf0\x9f\x92\xa2","angry",Y},
    {"collision","\xf0\x9f\x92\xa5","boom",Y},
    {"dizzy","\xf0\x9f\x92\xab","star",Y},
    {"sweat_drops","\xf0\x9f\x92\xa6","water splash",Y},
    {"dash","\xf0\x9f\x92\xa8","fast wind",Y},
    {"speech_balloon","\xf0\x9f\x92\xac","comment talk",Y},
    {"left_speech_bubble","\xf0\x9f\x97\xa8\xef\xb8\x8f","comment",Y},
    {"right_anger_bubble","\xf0\x9f\x97\xaf\xef\xb8\x8f","angry",Y},
    {"thought_balloon","\xf0\x9f\x92\xad","thinking",Y},
    {"zzz","\xf0\x9f\x92\xa4","sleep idle",Y},
    {"check_mark","\xe2\x9c\x94\xef\xb8\x8f","done yes",Y},
    {"white_check_mark","\xe2\x9c\x85","done yes pass",Y},
    {"ballot_box_with_check","\xe2\x98\x91\xef\xb8\x8f","done",Y},
    {"heavy_check_mark","\xe2\x9c\x94\xef\xb8\x8f","done",Y},
    {"x","\xe2\x9d\x8c","no fail wrong",Y},
    {"negative_squared_cross_mark","\xe2\x9d\x8e","no",Y},
    {"heavy_plus_sign","\xe2\x9e\x95","add plus",Y},
    {"heavy_minus_sign","\xe2\x9e\x96","minus remove",Y},
    {"heavy_division_sign","\xe2\x9e\x97","divide",Y},
    {"heavy_multiplication_x","\xe2\x9c\x96\xef\xb8\x8f","times",Y},
    {"infinity","\xe2\x99\xbe\xef\xb8\x8f","forever",Y},
    {"bangbang","\xe2\x80\xbc\xef\xb8\x8f","exclamation",Y},
    {"interrobang","\xe2\x81\x89\xef\xb8\x8f","",Y},
    {"question","\xe2\x9d\x93","help unknown",Y},
    {"grey_question","\xe2\x9d\x94","help",Y},
    {"exclamation","\xe2\x9d\x97","warning important",Y},
    {"grey_exclamation","\xe2\x9d\x95","",Y},
    {"warning","\xe2\x9a\xa0\xef\xb8\x8f","caution danger",Y},
    {"no_entry","\xe2\x9b\x94","blocked stop",Y},
    {"no_entry_sign","\xf0\x9f\x9a\xab","forbidden banned",Y},
    {"radioactive","\xe2\x98\xa2\xef\xb8\x8f","danger",Y},
    {"biohazard","\xe2\x98\xa3\xef\xb8\x8f","danger",Y},
    {"recycle","\xe2\x99\xbb\xef\xb8\x8f","retry reuse",Y},
    {"arrow_up","\xe2\xac\x86\xef\xb8\x8f","",Y},
    {"arrow_down","\xe2\xac\x87\xef\xb8\x8f","",Y},
    {"arrow_left","\xe2\xac\x85\xef\xb8\x8f","",Y},
    {"arrow_right","\xe2\x9e\xa1\xef\xb8\x8f","",Y},
    {"arrow_upper_right","\xe2\x86\x97\xef\xb8\x8f","",Y},
    {"arrow_lower_right","\xe2\x86\x98\xef\xb8\x8f","",Y},
    {"arrow_lower_left","\xe2\x86\x99\xef\xb8\x8f","",Y},
    {"arrow_upper_left","\xe2\x86\x96\xef\xb8\x8f","",Y},
    {"left_right_arrow","\xe2\x86\x94\xef\xb8\x8f","",Y},
    {"arrow_up_down","\xe2\x86\x95\xef\xb8\x8f","",Y},
    {"arrows_counterclockwise","\xf0\x9f\x94\x84","refresh sync retry",Y},
    {"arrows_clockwise","\xf0\x9f\x94\x83","refresh",Y},
    {"arrow_right_hook","\xe2\x86\xaa\xef\xb8\x8f","reply forward",Y},
    {"leftwards_arrow_with_hook","\xe2\x86\xa9\xef\xb8\x8f","reply back",Y},
    {"arrow_forward","\xe2\x96\xb6\xef\xb8\x8f","play",Y},
    {"arrow_backward","\xe2\x97\x80\xef\xb8\x8f","rewind",Y},
    {"fast_forward","\xe2\x8f\xa9","",Y},
    {"rewind","\xe2\x8f\xaa","",Y},
    {"black_right_pointing_double_triangle_with_vertical_bar","\xe2\x8f\xad\xef\xb8\x8f","next",Y},
    {"black_left_pointing_double_triangle_with_vertical_bar","\xe2\x8f\xae\xef\xb8\x8f","previous",Y},
    {"double_vertical_bar","\xe2\x8f\xb8\xef\xb8\x8f","pause",Y},
    {"black_square_for_stop","\xe2\x8f\xb9\xef\xb8\x8f","stop",Y},
    {"record_button","\xe2\x8f\xba\xef\xb8\x8f","record",Y},
    {"eject_button","\xe2\x8f\x8f\xef\xb8\x8f","eject",Y},
    {"repeat","\xf0\x9f\x94\x81","loop",Y},
    {"repeat_one","\xf0\x9f\x94\x82","",Y},
    {"twisted_rightwards_arrows","\xf0\x9f\x94\x80","shuffle",Y},
    {"new","\xf0\x9f\x86\x95","",Y},
    {"free","\xf0\x9f\x86\x93","",Y},
    {"up","\xf0\x9f\x86\x99","",Y},
    {"cool","\xf0\x9f\x86\x92","",Y},
    {"ng","\xf0\x9f\x86\x96","",Y},
    {"ok","\xf0\x9f\x86\x97","",Y},
    {"sos","\xf0\x9f\x86\x98","help emergency",Y},
    {"vs","\xf0\x9f\x86\x9a","versus",Y},
    {"id","\xf0\x9f\x86\x94","",Y},
    {"abc","\xf0\x9f\x94\xa4","",Y},
    {"abcd","\xf0\x9f\x94\xa1","",Y},
    {"capital_abcd","\xf0\x9f\x94\xa0","",Y},
    {"symbols","\xf0\x9f\x94\xa3","",Y},
    {"information_source","\xe2\x84\xb9\xef\xb8\x8f","info",Y},
    {"keycap_ten","\xf0\x9f\x94\x9f","10",Y},
    {"hash","\x23\xef\xb8\x8f\xe2\x83\xa3","number",Y},
    {"asterisk","\x2a\xef\xb8\x8f\xe2\x83\xa3","",Y},
    {"zero","\x30\xef\xb8\x8f\xe2\x83\xa3","",Y},
    {"one","\x31\xef\xb8\x8f\xe2\x83\xa3","",Y},
    {"two","\x32\xef\xb8\x8f\xe2\x83\xa3","",Y},
    {"three","\x33\xef\xb8\x8f\xe2\x83\xa3","",Y},
    {"four","\x34\xef\xb8\x8f\xe2\x83\xa3","",Y},
    {"five","\x35\xef\xb8\x8f\xe2\x83\xa3","",Y},
    {"six","\x36\xef\xb8\x8f\xe2\x83\xa3","",Y},
    {"seven","\x37\xef\xb8\x8f\xe2\x83\xa3","",Y},
    {"eight","\x38\xef\xb8\x8f\xe2\x83\xa3","",Y},
    {"nine","\x39\xef\xb8\x8f\xe2\x83\xa3","",Y},
    {"red_circle","\xf0\x9f\x94\xb4","",Y},
    {"orange_circle","\xf0\x9f\x9f\xa0","",Y},
    {"yellow_circle","\xf0\x9f\x9f\xa1","",Y},
    {"green_circle","\xf0\x9f\x9f\xa2","",Y},
    {"large_blue_circle","\xf0\x9f\x94\xb5","",Y},
    {"purple_circle","\xf0\x9f\x9f\xa3","",Y},
    {"brown_circle","\xf0\x9f\x9f\xa4","",Y},
    {"black_circle","\xe2\x9a\xab","",Y},
    {"white_circle","\xe2\x9a\xaa","",Y},
    {"red_square","\xf0\x9f\x9f\xa5","",Y},
    {"orange_square","\xf0\x9f\x9f\xa7","",Y},
    {"yellow_square","\xf0\x9f\x9f\xa8","",Y},
    {"green_square","\xf0\x9f\x9f\xa9","",Y},
    {"blue_square","\xf0\x9f\x9f\xa6","",Y},
    {"purple_square","\xf0\x9f\x9f\xaa","",Y},
    {"brown_square","\xf0\x9f\x9f\xab","",Y},
    {"black_large_square","\xe2\xac\x9b","",Y},
    {"white_large_square","\xe2\xac\x9c","",Y},
    {"small_red_triangle","\xf0\x9f\x94\xba","up",Y},
    {"small_red_triangle_down","\xf0\x9f\x94\xbb","down",Y},
    {"diamond_shape_with_a_dot_inside","\xf0\x9f\x92\xa0","",Y},
    {"radio_button","\xf0\x9f\x94\x98","",Y},
    {"eye","\xf0\x9f\x91\x81\xef\xb8\x8f","look watch",Y},
    {"eyes","\xf0\x9f\x91\x80","look watching",Y},
    {"wavy_dash","\xe3\x80\xb0\xef\xb8\x8f","",Y},
    {"curly_loop","\xe2\x9e\xb0","",Y},
    {"white_flower","\xf0\x9f\x92\xae","",Y},
    {"copyright","\xc2\xa9\xef\xb8\x8f","",Y},
    {"registered","\xc2\xae\xef\xb8\x8f","",Y},
    {"tm","\xe2\x84\xa2\xef\xb8\x8f","trademark",Y},
    {"checkered_flag","\xf0\x9f\x8f\x81","finish race",Y},
    {"triangular_flag_on_post","\xf0\x9f\x9a\xa9","flag",Y},
    {"crossed_flags","\xf0\x9f\x8e\x8c","",Y},
    {"black_flag","\xf0\x9f\x8f\xb4","",Y},
    {"white_flag","\xf0\x9f\x8f\xb3\xef\xb8\x8f","surrender",Y},
    {"rainbow_flag","\xf0\x9f\x8f\xb3\xef\xb8\x8f\xe2\x80\x8d\xf0\x9f\x8c\x88","pride lgbt",Y},
    {"pirate_flag","\xf0\x9f\x8f\xb4\xe2\x80\x8d\xe2\x98\xa0\xef\xb8\x8f","pirate",Y},
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
