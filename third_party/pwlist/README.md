# Common passwords of 15 characters or more

`common.txt`: every password of 15 to 128 printable ASCII characters in
SecLists' `Passwords/Common-Credentials/xato-net-10-million-passwords-1000000.txt`
(MIT, `LICENSE`), lowercased, deduplicated and sorted. Shorter ones are refused
by length already. The daemon refuses a new password found here
(`scripts/build/pw_common.py` compiles it in; AUTH.md §2).
