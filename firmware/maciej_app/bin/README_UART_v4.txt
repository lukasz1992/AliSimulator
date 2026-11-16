ELEKTRODA ALI M3801 TEST FOR MACIEJ - v4 (demo)

Aktualizacja UART dla bootloadera z dump_maciej.bin.
Wymagania: Python 3.10+, pyserial, UART 3,3 V (115200, 8E1).
Rozpakuj caly ZIP do jednego katalogu. Polacz TX-RX, RX-TX i GND.
Zamknij inne programy uzywajace portu tunera. COM3 zastap swoim portem.

python -m pip install pyserial
python maciej_uart.py --port COM3 --capture-boot 60 flash elektroda_ali_m3801_maciej_v4.maincode.bin --backup before-maciej-v4.bin

- Oryginalne oprogramowanie lub v1/v2: wlacz tuner po komunikacie gotowosci.
- Gdy dziala v3 lub v4: zostaw tuner wlaczony. Wysylane przez skrypt "c"
  uruchomi go ponownie i przechwyci oryginalny bootloader (v4 robi to po
  trzech znakach "c" w pustej linii polecen).
- Skrypt zapisze pelny backup, wgra maincode i zweryfikuje cale 4 MiB.
  Dwa odczyty trwaja lacznie okolo 14 minut. Nie wylaczaj tunera podczas zapisu.
- Do kolejnej aktualizacji uzyj nowej nazwy backupu. Stary nie jest nadpisywany.

v4 to demo z prawdziwa linia polecen: cale komendy zakonczone Enter.
Terminal 115200 8E1 (np. PuTTY), znak zachety "m3801> ".
Edycja linii jak w bashu: strzalki lewo/prawo, Home/End (Ctrl-A/Ctrl-E),
Ctrl-strzalki (Alt-B/Alt-F) o slowo, Backspace, Delete (Ctrl-D), Ctrl-W
kasuje slowo, Ctrl-U wszystko przed kursorem, Ctrl-K wszystko za nim,
Ctrl-Y wkleja skasowane, Ctrl-L czysci ekran, Ctrl-C porzuca linie.
Strzalki gora/dol przywracaja poprzednie komendy zaczynajace sie od tego,
co juz wpisano (64 ostatnie, bez powtorzen); Ctrl-R szuka komendy
zawierajacej wpisywany tekst; !! powtarza ostatnia, !n komende numer n z
'history', !-n n-ta od konca, !tekst ostatnia zaczynajaca sie od tekstu.
Tab dopelnia nazwe komendy, jej podkomende (np. "rf sp" -> "rf spectrum")
i nazwe pliku albo katalogu na dysku ("ls 4:/c6" -> "ls 4:/c64/");
gdy pasuje kilka, wypisuje je (dlugie listy po drugim Tab).
Komende mozna tez zaczac od '/' (np. /restart, /lsusb).

  help                 lista komend (help <komenda> - opis jednej)
  version / ping       banner i wersja / odpowiedz "pong v4"
  info / uptime        uklad, zegar CPU, adresy w pamieci, czas pracy
  echo <slowa>         wypisuje argumenty
  md <adres> [dl]      zrzut pamieci (RAM, okno flash, rejestry)
  mr <adres>           odczyt slowa 32-bit
  mw <adres> <wart>    zapis slowa 32-bit (RAM i rejestry, nie flash)
  screen ...           rysuje na ekranie testowym (i go pokazuje): demo | bars |
                       clear [rrggbb] | text <x> <y> <slowa> |
                       rect <x> <y> <w> <h> <rrggbb> | on | off
  testscreen           ekran testowy na TV (ramki, paski kolorow); dowolny
                       klawisz klawiatury USB (wpisuje sie tez) albo
                       "console" wraca do konsoli
  console              konsola na TV (widok po starcie)
  explorer [n:][sciezka]
                       menedzer plikow na TV w biezacym katalogu, zgrany z
                       "cd" w obie strony: strzalki/PgUp/PgDn wybieraja,
                       Enter otwiera (obraz: przegladarka, plik .nes albo
                       .gb: gra, inny plik: jego poczatek), Backspace w gore,
                       "/" albo Tab - wpisywanie sciezki w pasku; mysz:
                       klik, podwojny klik, kolko; Esc wraca do konsoli;
                       F2 zmienia nazwe, F7 nowy katalog, Delete/F8
                       usuwa (y), F5/F6 kopiuj/przenies plik, Insert wkleja
  panel <tekst>        4 znaki na wyswietlaczu panelu; panel bright 0..7
                       (plyty z TM1650)
  keys                 stan klawiszy panelu (klawisze sa tez zglaszane same);
                       bez TM1650: dwa przyciski na GPIO 31 i 9
  ir                   ostatni kod pilota; ir raw on|off - czasy impulsow
  usb                  port USB: stan, predkosc i deskryptory podlaczonego
                       urzadzenia (VID:PID, nazwy, interfejsy); usb reset -
                       reset magistrali, usb init - kontroler od nowa,
                       usb kbd [report|interrupt|raw on|off] - klawiatura,
                       usb mouse - mysz (kursor, przyciski, liczniki),
                       usb reg <offset> [bajt] - rejestr kontrolera,
                       usb ctl <typ> <zadanie> <wartosc> <indeks> <dlugosc>
                       [urzadzenie] - dowolne zadanie kontrolne do urzadzenia
                       o danym adresie (np. usb ctl 0x80 6 0x100 0 18 2);
                       zadanie OUT wysyla bajty z ostatniego odczytu
  lsusb                urzadzenia USB, po jednej linii
  disks                dyski USB (pendrive, czytnik kart): rozmiar, partycje,
                       systemy plikow
  ls [n:][sciezka]     katalog na dysku USB (FAT12, FAT16, FAT32, NTFS, dlugie
                       nazwy); n - numer urzadzenia z lsusb (np. ls 3:/docs),
                       bez niego biezacy katalog (cd), a przed pierwszym cd
                       pierwszy wolumin FAT; "dir" to samo
  cd [n:][sciezka]     biezacy katalog, widoczny w znaku zachety
                       (m3801 3:/Docs>); cd .. - wyzej, cd / - korzen,
                       samo cd - bez katalogu; pwd - pokazuje go
  cat [n:]sciezka      plik tekstowy z dysku USB (Ctrl-C przerywa; plik
                       binarny: pierwsze 256 bajtow szesnastkowo)
  mkdir <sciezka>      nowy katalog (ZAPIS na dysk; dlugie nazwy tez)
  rmdir <sciezka>      usuwa pusty katalog (zapis)
  rm [-r] <sciezka>    usuwa plik; -r: katalog z cala zawartoscia (zapis)
  touch <sciezka>      nowy pusty plik (zapis)
  cp [-r] <skad> <dokad>
                       kopiuje plik (do katalogu: do niego, pod ta sama
                       nazwa; takze na inny dysk); -r: katalog z cala
                       zawartoscia (zapis)
  mv <skad> <dokad>    zmienia nazwe pliku lub katalogu albo przenosi go
                       do innego katalogu tego samego woluminu (zapis)
  komenda > plik       to, co komenda wypisuje, trafia do pliku (np.
                       ls > 4:/lista.txt, echo tekst > 4:/a.txt); >> dopisuje
                       na koncu pliku; nazwy ze spacjami w "..." albo '...'
  sector [n:]<numer>   jeden sektor dysku USB szesnastkowo
  sum [n:]sciezka      plik przeczytany do konca: rozmiar, CRC-32 (jak 7-Zip),
                       pierwsze bajty, predkosc (Ctrl-C przerywa)
  head [-n N] <plik>   pierwsze N linii pliku (10); tail - ostatnie N
  df [n:]              woluminy dyskow: rozmiar, wolne i zajete miejsce
  ntfs [n:] [reset-journal]
                       wolumin NTFS: geometria, stan dziennika i czy demo go
                       zapisuje (tylko wolumin zamkniety czysto przez Windows:
                       "Bezpieczne usuwanie"); reset-journal czysci dziennik
                       tak jak ntfs-3g
  view [-1] <plik>     obraz na TV (JPEG zwykly i progresywny, PNG, GIF, BMP),
                       dopasowany do ekranu, obrocony wg EXIF zdjecia;
                       strzalka w prawo / spacja - nastepny obraz z katalogu,
                       w lewo - poprzedni, i - szczegoly, Esc albo q konczy
  play [-n klatki] [-x] [-k] [-v] [-1] <plik>
                       film z pliku (.ts, .mpg, .m2v, .vob: wideo MPEG-2 albo
                       MPEG-1, dzwiek MPEG) na TV, dekodowany sprzetowym
                       dekoderem MPEG-2 ukladu; dzwiek przez "snd". Esc,
                       Ctrl-C, q albo EXIT na pilocie konczy; -n tyle klatek,
                       -x tak szybko jak dekoduje (bez dzwieku), -k ostatnia
                       klatka zostaje na TV, -v kazda klatka opisana, -1
                       obraz w swoim rozmiarze (zamiast na wysokosc ekranu)
  av <polecenie>       dekoder wideo i warstwa obrazu: av status (rejestry),
                       av shot <plik.bmp|plik.yuv> (ostatnia klatka do
                       pliku), av show, av off (z powrotem menu), av reset
  gb [-f klatki] <plik.gb>
                       gra Game Boy albo Game Boy Color na TV (gra na Color
                       w kolorach, starsza w czterech odcieniach), 59,7
                       klatki na sekunde (kartridze bez MBC, MBC1, MBC2,
                       MBC3, MBC5; bez dzwieku): klawisze jak w nes;
                       pamiec z bateria w pliku .sav obok gry
  nes [-f klatki] <plik.nes>
                       gra NES na TV, 60 klatek na sekunde (mappery 0, 1, 2,
                       3, 4, 7, 11, 34, 66, 71; bez dzwieku): strzalki albo
                       W A S D, X = A, Z = B, Enter = Start, Tab = Select
                       (klawiatura USB, UART, pilot: strzalki, OK = A, 0 = B,
                       MENU = Start, INFO = Select); Ctrl-C albo Esc konczy;
                       pamiec baterii gry w pliku .sav obok niej
  sms [-f klatki] <plik.sms>
                       gra Sega Master System na TV (256 x 192, 3 razy
                       wiekszy; mappery Sega i Codemasters, pamiec kartridza
                       w pliku .sav; bez dzwieku): X = przycisk 2, Z =
                       przycisk 1, Enter = Pause; reszta jak w nes
  gg [-f klatki] <plik.gg>
                       gra Sega Game Gear na TV (160 x 144, 5 razy wiekszy):
                       X = przycisk 2, Z = przycisk 1, Enter = Start
  zx [-f klatki] [plik]
                       ZX Spectrum 48K na TV (320 x 240 z ramka, 3 razy
                       wiekszy, 50 klatek/s): snapshot (.sna, .z80) albo
                       tasma (.tap, .tzx - LOAD "" wpisuje sie samo); bez
                       pliku: BASIC. ROM Spectrum to plik 48.rom (nie ma go
                       w demie) obok programu, w katalogu zx/ albo na
                       szczycie dysku. Klawiatura USB jest klawiatura
                       Spectrum (Shift = CAPS SHIFT, Ctrl / Alt = SYMBOL
                       SHIFT, Backspace = DELETE), strzalki to tez joystick
                       Kempston (ogien: OK na pilocie); znaki wpisane przez
                       UART / telnet sa na nim wpisywane; Esc konczy
  a26 [-f klatki] <plik.a26>
                       gra Atari 2600 na TV (160 x 228, 5 razy szerszy i 3
                       razy wyzszy; kartridze 2K, 4K, F8, F6, F4 i ich RAM
                       Superchip, FE, E0, 3F, E7, FA; NTSC; bez dzwieku):
                       strzalki, X albo Z = ogien, Enter = Reset konsoli,
                       Tab = Select
  c64 [-f klatki] [plik]
                       Commodore 64 (PAL) na TV (352 x 240 z ramka, 3 razy
                       wiekszy, 50 klatek/s): program (.prg, .t64) albo
                       dyskietka (.d64) - wczytuje sie i startuje (RUN) sam,
                       LOAD z dyskietki dziala tez pozniej; bez pliku: BASIC.
                       ROM-y C64 to pliki kernal.rom, basic.rom i chargen.rom
                       (nie ma ich w demie) obok programu, w katalogu c64/
                       albo na szczycie dysku. Klawiatura USB jest klawiatura
                       C64 (Tab = RUN/STOP, PageUp = RESTORE, Alt = C=),
                       strzalki to joystick w porcie 2 (ogien: prawy Ctrl,
                       OK na pilocie; F9 zamienia porty); MENU na pilocie =
                       SPACJA, INFO = F7; znaki wpisane przez UART / telnet
                       sa na nim wpisywane; Esc konczy
  amiga [-f klatki] [plik.adf]
                       Amiga 500 (PAL, OCS) na TV (352 x 272 z ramka, 2 razy
                       wiekszy) z obrazem dyskietki (.adf) w stacji (bez
                       pliku: pusta stacja). Kickstart to plik kick.rom (256
                       albo 512 KB, nie ma go w demie) albo darmowy AROS:
                       aros-rom.bin + aros-ext.bin - obok dyskietki, w
                       katalogu amiga/ albo na szczycie dysku. Klawiatura USB
                       jest klawiatura Amigi, strzalki to joystick (ogien:
                       prawy Ctrl, OK na pilocie), lewy przycisk myszy: Page
                       Up / Page Down albo 0 na pilocie; MENU = Return, INFO
                       = spacja; bez dzwieku; Esc konczy
  dos [-f klatki] <plik.exe>
                       PC z DOS-em (286 AT z karta VGA: tryby tekstowe,
                       CGA, EGA, 256 kolorow) na TV (320 x 200, 3 razy
                       wiekszy) uruchamiajacy program DOS (.exe, .com);
                       BIOS i DOS sa wbudowane, bez dzwieku. Klawiatura
                       USB jest klawiatura PC (Esc konczy); pilot: strzalki,
                       OK = gora, 0 = spacja, MENU = Enter, INFO = Esc
  snd <polecenie>      dzwiek (prototyp): snd init, snd beep [Hz] [ms] (ton
                       testowy), snd vol 0..100, snd status, snd stop -
                       48 kHz na HDMI (po "video init") i na SCART (L/P)
  rf <polecenie>       wejscie antenowe (DVB-T): tuner MxL603 (I2C 0x60)
                       i demodulator S3811 ustawiane tak jak robi to
                       oryginalny soft dump.bin: rf init, rf tune <MHz albo
                       kanal> [6|7|8], rf status (lock, poziom, jakosc,
                       bledy), rf scan, rf services (nazwy kanalow
                       multipleksu), rf ts <s> [plik] (PID-y albo nagranie
                       .ts), rf spectrum (poziom sygnalu z anteny od 174 do
                       862 MHz jako wykres na TV); tylko DVB-T, nie DVB-T2
  tty                  porty szeregowe USB: porty modulu GSM / LTE (SIMCom,
                       Quectel, Huawei, ZTE ...), urzadzenia CDC ACM
                       i przejsciowki USB-UART (CH340 / CH341, CP210x, FTDI,
                       PL2303)
  stty [n] [baud]      predkosc portu szeregowego (8N1): pokazuje albo
                       ustawia, np. stty 0 9600 (przejsciowki startuja
                       z 115200)
  at [komenda]         jedna komenda AT do portu AT modulu i jej odpowiedz do
                       OK / ERROR oraz to, co przychodzi zaraz po nich (dane
                       AT+HTTPREAD); najpierw linie, ktore modul wyslal
                       wczesniej (+HTTPACTION, RING); np. at +csq, at i,
                       at +cpin?; linia zaczynajaca sie od AT tez tam trafia;
                       dziala > plik
  cu [n]               terminal polaczony z portem szeregowym n (bez n: port
                       AT modulu): co wpiszesz, idzie do portu, odpowiedzi
                       widac; "~." na poczatku linii albo Ctrl-] konczy
  ifconfig             lacze sieciowe: adapter Ethernet USB (CDC ECM, np.
                       RTL8153) albo modul LTE (RNDIS, np. SIMCom A7670E),
                       gdy adaptera nie ma albo nie dostal adresu: MAC,
                       adres IP z DHCP, router
  dhcp                 ponownie prosi serwer DHCP o adres
  arp                  tabela ARP
  ping [-c n] <host>   ping jak w Linuksie przez siec: adres albo nazwa
                       (DNS z DHCP), TTL i czas kazdej odpowiedzi, Ctrl-C
                       konczy i pokazuje statystyke (samo "ping": pong)
  wget [-O n:plik] <url>
                       strona http:// przez siec: na konsoli albo
                       zapisana na dysku USB (https nieobslugiwane)
  ntp [serwer] / date  zegar z sieci (sam po DHCP) / data i godzina: UTC
                       i polska (CET / CEST)
  netstat              TCP: porty, polaczenia, liczniki
  loop                 petla glowna: przebiegi, najwolniejszy, co trwalo
  Przez siec: telnet <adres tunera> (port 23) - ta sama linia polecen co
  na UART; python tools/maciej_netload.py <adres> [app.bin] - nowa wersja
  demo uruchamiana z RAM w ~1 s (flash bez zmian, restart wraca do niej);
  ftp://<adres tunera> (port 21, bez hasla, 2 klienty) - serwer FTP: w katalogu
  glownym wszystkie dyski ("4 PHONE CARD", "9 ..."), pobieranie,
  wysylanie, usuwanie, zmiana nazw i katalogi na FAT i NTFS (NTFS gdy demo
  go zapisuje: "ntfs"); tryb pasywny (porty danych 50021/50022) i aktywny.
  http://<adres tunera> (port 80, bez hasla) - strona tunera w przegladarce
  (takze na telefonie): pliki na dyskach (przegladanie, pobieranie,
  wysylanie przyciskiem albo przeciagnieciem, zmiana nazw, usuwanie, nowe
  katalogi; obrazy i teksty otwieraja sie w przegladarce), obraz z TV na
  zywo (co ok. 1 s; "Full size" - pelne 1280 x 720), pilot na ekranie
  (strzalki, OK, Esc, Tab, A/B do gier, Ctrl-C) i linia polecen jak przez
  telnet. Gdy dziala polecenie (np. gra), obraz, pilot i linia polecen
  dzialaja dalej, a pliki czekaja na jego koniec; jeden plik naraz
  (strona albo FTP - drugi dostaje "sprobuj za chwile").
  Data i godzina z NTP: w prawym gornym rogu konsoli na TV, po czasie pracy
  watchdog [test|off|on]
                       watchdog ukladu: dziala od startu demo; gdy demo stanie
                       na 8 s, tuner sam sie restartuje; test - przestaje go
                       karmic (tuner restartuje sie po 8 s)
  clear                czysci ekran terminala (i konsoli na TV)
  history [n | -c]     poprzednie komendy z numerami (n ostatnich; -c
                       czysci historie)
  video init [opticum|comsat] [rev0|rev1]
                       odtwarza zapisy rejestrow, ktorymi oryginalne
                       oprogramowanie plyty wlacza obraz 720p i nadajnik HDMI
                       (demo robi to samo przy starcie);
                       bez nazwy - tabela tej plyty, bez rev0/rev1 - tabela
                       wersji ukladu (bajt 0xB8000000)
  reboot / restart     restart przez oryginalny bootloader
  bootloader           restart do oryginalnego bootloadera: przytrzymaj klawisz c
                       (albo uruchom maciej_uart.py), zeby w nim zostac

Konsola na TV (widok po starcie): to samo co na UART - komunikaty startowe,
znak zachety, wpisywane komendy (z UART albo klawiatury USB), ich wyniki
i zdarzenia ([usb] ..., [panel] ...) - jak konsola w Linuksie: 105 x 42
znakow, pasek tytulu, znak zachety na zielono, kursor miga. Linie przewijaja
sie w gore; ostatnie 512 linii zostaje - PgUp / PgDn (klawiatura USB albo
ESC [ 5 ~ / ESC [ 6 ~ z terminala) przewija o pol ekranu. "testscreen"
pokazuje dawny ekran demo (ekran testowy), klawisz na klawiaturze USB albo
"console" wraca do konsoli.
Zapis na dyski: mkdir, rmdir, rm, touch, cp, mv i "komenda > plik" zapisuja
wolumin FAT (wszystkie kopie FAT, dlugie nazwy, FSInfo w FAT32; pliki
tekstowe maja konce linii jak w Linuksie) i NTFS - ten tylko wtedy, gdy
Windows zamknal go czysto (pusty dziennik: "Bezpieczne usuwanie" przed
wyjeciem; "ntfs n:" mowi, czy demo go zapisuje). Po zapisie przez demo
warto sprawdzic pendrive w Windows (chkdsk). Tuner nie ma zegara - nowe pliki
i katalogi maja date 2026-11-13 12:00. Dysk chroniony przed zapisem (MODE
SENSE) nie jest zapisywany. Uwaga: zapis zmienia dane na pendrivie - najpierw
sprobuj na pendrivie bez waznych plikow.
Modul GSM / LTE (sprawdzone z SIMCom A7670E, 1e0e:9011): jego porty to
ttyUSB0..3 ("tty"), "at +csq" pyta o zasieg, "cu" otwiera port AT jak
terminal. Gdy modul sie zrestartuje (np. po AT+CRESET), demo samo
konfiguruje go ponownie; pojedynczy blad transmisji USB (np. gdy modul nadaje
w GSM) nie przerywa "at" ani "cu". Karta SIM z PIN-em: at +cpin=<PIN> (albo
wylacz PIN w telefonie).
Internet przez modul (jego funkcja RNDIS, jak usb0 w Linuksie): gdy nie ma
adaptera Ethernet, gdy zostanie odlaczony albo nie dostanie adresu z DHCP,
siec demo (ping, wget, ntp, telnet, FTP, netload) idzie przez modul; jego
DHCP daje adres, a jego router wyjscie do sieci komorkowej. "ifconfig"
pokazuje, ktore lacze jest w uzyciu. (Sprawdzone w emulatorze; modul musi
miec aktywne polaczenie danych.)
Strona WWW przez modul (jego wlasny klient HTTP):
  at +cgact=1,1                              (polaczenie danych)
  at +httpinit
  at +httppara="URL","http://example.com/"
  at +httpaction=0                           (po kilku sekundach modul wysle
                                              +HTTPACTION: 0,200,<dlugosc>)
  at +httpread=0,<dlugosc>                   (strona; mozna tez > plik)
  at +httpterm
Sprawdzone przez GSM: example.com, 577 bajtow.

Pilot: kazda odebrana ramka NEC jest wypisywana z kodem i nazwa klawisza,
pokazana na ekranie i na panelu. Klawisze panelu: wypisywane po nacisnieciu.
USB: po podlaczeniu urzadzenia demo samo odczytuje jego deskryptory
i pokazuje nazwe na ekranie. Pendrive i czytniki kart (mass storage, SCSI,
bulk-only) sa od razu sprawdzane: nazwa, rozmiar, partycje (MBR lub GPT)
i system plikow. Pliki mozna czytac i zapisywac na FAT12/16/32 i NTFS;
exFAT jest rozpoznawany, ale nie czytany. Sprawdzone na Comsat TE 1050 HD:
karta FAT16 w czytniku 05e3:0751 i pendrive NTFS 64 GB.
Przejsciowki USB-UART (CH340 / CH341 w trybie szeregowym, CP210x, FTDI,
PL2303): kazda jest portem ttyUSB<n> (115200 8N1), "stty n <baud>" zmienia
predkosc, "cu n" otwiera terminal. Sprawdzone: CH340 1a86:7523 (ustawianie
predkosci). CH341A w trybie I2C/SPI (1a86:5512, programator 24xx/25xx) jest
tylko rozpoznawany.
Obrazy: "view plik" albo Enter w eksploratorze; zdjecie 2000 x 1333 JPEG
dekoduje sie okolo 1,5 s. Gry NES: "nes plik.nes" albo Enter w
eksploratorze; dziala pelne 60 klatek/s (klatka ok. 8 ms + 1,5 ms rysowania).
Gier komercyjnych (np. Mario) demo nie zawiera - mozna uzyc wlasnych kopii
albo darmowych gier homebrew (np. Nova the Squirrel, Thwaite).
Gry Game Boy i Game Boy Color: "gb plik.gb" (albo .gbc) lub Enter w
eksploratorze; klatka ok. 6-7 ms + 0,7 ms rysowania (gra na Color w podwojnej
predkosci ok. 12 ms + 1,5 ms), obraz 5 razy wiekszy (800 x 720).
Gry Sega Master System i Game Gear: "sms plik.sms", "gg plik.gg" albo Enter
w eksploratorze; klatka ok. 4-5 ms + 1-2 ms rysowania. ZX Spectrum 48K: "zx
plik.tap" (albo .tzx, .sna, .z80) lub "zx" bez pliku (BASIC); potrzebny jest
plik 48.rom (ROM Spectrum, 16 KB - Amstrad pozwala go uzywac w emulatorach)
obok programu, w zx/ albo na szczycie dysku; klatka ok. 4 ms + 2,5 ms.
Tasmy ladowane sa od razu przez procedure LD-BYTES z ROM-u, wiec gry z
wlasnym turbo-loaderem sie nie wczytaja. Atari 2600: "a26 plik.a26" albo
Enter w eksploratorze; klatka ok. 3,5-5,5 ms + 1 ms. Sprawdzone na Comsat TE
1050 HD z darmowymi grami homebrew (np. Snake Pit, Sushi Nights, Ramiro el
Vampiro, Halo 2600, Thrust).
Commodore 64: "c64 plik.t64" (albo .prg, .d64) lub "c64" bez pliku (BASIC)
albo Enter w eksploratorze; potrzebne sa ROM-y kernal.rom (8 KB), basic.rom
(8 KB) i chargen.rom (4 KB) obok programu, w c64/ albo na szczycie dysku
(pliki z VICE pod tymi nazwami). LOAD jest robiony od razu (bez stacji
1541), wiec gry z wlasnym szybkim loaderem dzialajacym w stacji sie nie
wczytaja. Klatka ok. 3-4 ms + 3-5 ms rysowania. Sprawdzone na Comsacie:
Magicland Dizzy (wersja .t64 z trainerem), Hessian (.d64).
Dzwiek (prototyp, jeszcze nie sluchany): uklad dzwiekowy S3811 ustawiany tak
jak robi to oryginalny soft (dump.bin), 48 kHz na HDMI i na wyjsciu SCART.
Gry NES graja z dzwiekiem (oba kanaly prostokatne, trojkat, szum, DMC). Na
pudelku sprawdzone tylko po licznikach (sprzet pobiera probki z predkoscia
48000/s, nic nie ginie). Test: "video init", "snd init", "snd beep 1000 3000".
Filmy: "play plik.ts" albo Enter w eksploratorze (.ts, .mpg, .m2v, .vob).
Obraz dekoduje sprzetowy dekoder MPEG-1/2 ukladu S3811 (ten sam, ktorego
uzywa oryginalny soft do nagran), dzwiek MPEG (Layer II) dekodowany
programowo, zmiksowany do mono. Na pudelku: plik 720 x 576, 25 klatek/s,
6 Mbit/s gra w czasie rzeczywistym (500 klatek w 20,3 s, 0 bledow), klatki
odczytane z pamieci zgodne z ffmpeg; 1920 x 1080 tez sie dekoduje (do
24 ms na klatke), ale karta czyta ok. 1 MB/s, wiec duze pliki HD zwalniaja.
Skaler obrazu bierze filtry z tablicy w RAM-ie (rejestr +0x1E4 wskazuje
blok parametrow) - player wpisuje je sam, policzone tak jak w oryginalnym
sofcie; bez nich obraz na TV byl rozsypany, choc klatki w pamieci dobre.
Warstwa obrazu czyta klatke linia po linii (tryb 1 w +0x190, bity 17-18);
w trybie pol (0) obraz mial polowe wysokosci, a pod nim byly smieci.
Obraz na TV sprawdzony okiem (kamera telefonu): caly, kolory dobre.
H.264 (DVB-T2, wiekszosc nowych nagran) nie jest obslugiwane - uklad ma do
tego osobny silnik, jeszcze nie oprogramowany. Pliki wrzucac przez FTP.
PC z DOS-em: "dos plik.exe" albo Enter w eksploratorze (.exe, .com); jeden
program bez plikow danych obok (Magicland Dizzy z 1993 ich nie potrzebuje:
spacja na ekranie ustawien, Z / X albo strzalki - chodzenie, K albo gora -
skok, spacja albo Enter - podnies / uzyj). Klatka ok. 2,5-4 ms + 2-2,5 ms
rysowania (z 14,3 ms). Sprawdzone na Comsacie: Magicland Dizzy (DOS).
Amiga 500: "amiga plik.adf" albo Enter w eksploratorze; potrzebny jest
Kickstart (kick.rom) albo darmowy zamiennik AROS (aros-rom.bin i aros-ext.bin
ze strony aros.sourceforge.io) w amiga/. Dyskietka jest czytana tak jak
prawdziwa stacja (sciezki MFM), wiec dzialaja tez gry z wlasnym loaderem.
Klatka ok. 5,5-9 ms + 3-4 ms rysowania. Sprawdzone na Comsacie: Magicland
Dizzy (wersja TRSi) z AROS-em - start ok. 30 s, intro wymaga klikniecia
myszka (0 na pilocie albo Page Down).
Wejscie antenowe ("rf"): tuner MaxLinear MxL603 na I2C 0x60 (SCB0) i
demodulator COFDM wbudowany w S3811 - ustawiane rejestr w rejestr tak jak
robi to oryginalny soft tunera (dump.bin). "rf scan" przeszukuje kanaly
21-69, "rf services" czyta nazwy kanalow (PAT, SDT), "rf ts 10 4:/x.ts"
nagrywa caly multipleks do pliku (do odtworzenia np. w VLC). Uwaga: S3811
odbiera tylko DVB-T, a w Polsce multipleksy nadaja juz w DVB-T2, wiec na
Comsacie w Polsce nie ma czego zlapac: tuner dziala i widzi sygnaly, ale
demodulator nie zlapie DVB-T2. "rf spectrum" rysuje na TV poziom sygnalu z
anteny od 174 do 862 MHz (ok. 35 s) - widac na nim multipleksy DVB-T2 i LTE
800.
Hub USB: demo wlacza zasilanie portow huba, resetuje kazdy port z urzadzeniem
i odczytuje urzadzenia za hubem (takze hub za hubem), razem do 15 urzadzen
(hub musi miec jeden z pierwszych 7 adresow);
"lsusb" pokazuje kazde z numerem huba i portu. Podlaczenie i odlaczenie
urzadzenia w hubie jest zauwazane w ciagu okolo sekundy. Urzadzenia low
i full speed za hubem USB 2.0 sa obslugiwane przez translator huba (split).
Sprawdzone na Comsat TE 1050 HD z hubem 7-portowym (2 x 1a40:0101): hub,
drugi hub, trzy pamieci USB i klawiatura sa wykrywane, klawiatura za hubem
pisze.
Klawiatura USB (HID, protokol boot) pisze w tej samej linii polecen co UART:
uklad US, Enter, Backspace, strzalki gora/dol, Caps Lock, Esc = Ctrl-C,
przytrzymany klawisz sie powtarza. Wpisywana linie widac tez na TV (ramka
COMMAND LINE). Klawisze czytane sa z endpointu przerwan klawiatury, tak jak
robi to PC: kontroler pyta ja co 1 ms i dostaje raport, gdy zmieni sie stan
klawiszy. "usb kbd report" przelacza na odczyt przez GET_REPORT co 16 ms,
"usb kbd interrupt" z powrotem (demo przelacza sie tez samo, gdy wybrany
sposob jest odrzucany). Nie kazda klawiatura podaje klawisze przez GET_REPORT:
sprawdzona klawiatura low-speed 1a2c:2d23 odpowiada na nie zawsze zerami.
Pisanie na tej klawiaturze sprawdzone na Comsat TE 1050 HD.
Mysz USB (HID, protokol boot) przesuwa kursor (strzalke) po obrazie na TV,
zaczynajac od srodka. Z wcisnietym lewym przyciskiem rysuje linie, prawy
przycisk wybiera nastepny kolor (zolty, czerwony, zielony, niebieski, bialy,
pomaranczowy); przyciski i kolko sa wypisywane na UART z pozycja kursora.
Mysz odpytuje sam kontroler (jego endpoint D, co 8 ms), wiec nie przeszkadza
klawiaturze ani dyskom USB. "usb mouse" pokazuje pozycje kursora i liczniki.
Sprawdzone na Comsat TE 1050 HD: mysz A4Tech 09da:c10a (low speed) za hubem.

Plyty: Opticum HD N2 (dump_maciej.bin, panel TM1650 na GPIO 31 i 9) oraz
Comsat TE 1050 HD (dump.bin: bez wyswietlacza, dwa przyciski na tych samych
pinach, zwierane do masy). Gdy TM1650 nie odpowiada, demo przestaje sterowac
tymi pinami i czyta je jako przyciski UP (GPIO 31) i DOWN (GPIO 9).

Obraz: aplikacja rysuje 1280x720 (ARGB1555) na warstwie OSD. Na S3811
w wersji 1 naglowek warstwy (0x280 bajtow) niesie macierz RGB->YCbCr, tak jak
w oryginalnych aplikacjach; bez niej kazdy kolor bylby czarny. Bootloader nie
wlacza wyjscia wideo, wiec demo przy starcie samo odtwarza ustawienia obrazu
swojej plyty (to samo co "video init"), a dopiero potem USB - tak jak oryginal,
bo te zapisy rozlaczaja urzadzenie USB (po "video init" demo ustawia je
ponownie). Sterownik HDMI oryginalu uruchamia nadajnik dopiero, gdy wykryje
telewizor, dlatego tabele nagrano w emulatorze z "podlaczonym" telewizorem:
obejmuja start nadajnika HDMI (wyjscie z resetu, wylaczenie wyciszenia)
i koncza sie 1,5 s pozniej. Sprawdzone na Comsat TE 1050 HD (S3811 w wersji 1):
obraz przez HDMI, kolory poprawne.
Gdyby demo sie zawiesilo, watchdog po 8 s sam uruchomi tuner ponownie przez
oryginalny bootloader (skrypt aktualizacji zlapie go bez wylaczania zasilania).
Gdyby i to nie pomoglo: wylacz zasilanie i wlacz ponownie.

Do UART sluzy .maincode.bin. Bootloader, klucze i ustawienia zostaja zachowane.
Nie potrzeba szyfrowania - uzywane sa LZMA i CRC.
