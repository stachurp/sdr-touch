# SDR Touch

Prototyp odbiornika i skanera dla SDRplay RSP1 na Raspberry Pi z dekodowaniem FT8, FT4, WSPR, DMR, Yaesu System Fusion i ADS-B.
Nie obsługuje nadawania. Program korzysta z SoapySDR z modułem SDRplay, WDSP, FFTW3f, Qt 5 i ICU,
a do dekodowania i informacji o krajach także z pakietów `wsjtx`, `iso-codes` i `fonts-noto-color-emoji`.

## Uruchomienie

Z menu pulpitu wybierz **SDR Touch**. Aby uruchomić program przez SSH na podłączonym ekranie
przy obecnej konfiguracji sesji użytkownika `piasek`, wykonaj:

```sh
XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland ~/sdr-touch/build/sdr-touch
```

Polecenie pozostaje uruchomione do zamknięcia okna. Przed startem zamknij inne programy
korzystające z RSP1. Jeśli używasz innej sesji graficznej, dostosuj zmienne środowiskowe.

Do próby odsłuchu ustaw znaną częstotliwość lokalnej stacji radiowej i wybierz **WFM mono**.
Sprawdź antenę i wyjście dźwięku w ustawieniach pulpitu. Dostępne są również AM, NFM, LSB i USB.
Zakres przestrajania wynosi 10 kHz–2 GHz; nie oznacza to jednakowej czułości w całym zakresie.
WFM ma 50 µs de-emfazy, jest monofoniczne i nie dekoduje RDS.

Na ekranie **Odbiornik** przycisk **Wycisz** wyłącza tylko dźwięk głośnika; ponowne
naciśnięcie przywraca ustawioną głośność. Odbiór, wskaźnik poziomu i dekoder nadal działają.
Wskaźnik pokazuje szczyt FFT w paśmie odbioru, a nie potwierdzenie obecności transmisji FT8.
Wybierz zapisaną listę i częstotliwość lub użyj przycisków ◀ / ▶, aby przełączać pozycje.
Razem z częstotliwością ustawiana jest jej modulacja. Pozycja z inną modulacją niż wymagana
przez aktywny dekoder (USB dla FT8/FT4/WSPR, NFM dla DMR/YSF) wyłącza dekoder.

Dotknięcie edytowalnego pola tekstowego lub liczbowego otwiera klawiaturę ekranową.
Wpis zatwierdza się przyciskiem **Gotowe** albo odrzuca przez **Anuluj**.

## Skanowanie i listy

W zakładce **Skanowanie i listy** ustaw modulację, początek, koniec, krok i próg, a następnie
naciśnij **Rozpocznij skanowanie**.
Skan przebiega jednokrotnie, z oczekiwaniem 250 ms na krok (400 ms dla WFM) plus czas
pomiarów i przestrajania. Jeden tuner oznacza,
że odsłuch jest wstrzymany na czas skanowania. **Zatrzymaj skanowanie** przywraca odsłuch.

Poziom to szczyt FFT w paśmie wybranego trybu, w przybliżonych dBFS, przy włączonym AGC
sprzętowym. Nie jest kalibrowanym pomiarem dBm; próg należy dobrać do szumu i anteny.
Wynik wymaga przekroczenia progu i wyraźnego sygnału ponad tło w co najmniej dziewięciu z piętnastu
kolejnych pomiarów. Skan WFM porównuje także poziom w kanale z sąsiednim tłem.
Ogranicza to przypadkowy szum i pojedyncze impulsy, ale nie potwierdza transmisji:
zakłócenia mogą zostać wykryte, a słabe lub krótkie transmisje pominięte. Silna stacja
w sąsiednim kanale może utrudnić wykrycie słabszej.
Zapisana częstotliwość jest punktem siatki skanowania, a nie dokładnym pomiarem częstotliwości
sygnału. Szerokie pasmo WFM i mały krok mogą powodować wiele wyników dla jednego sygnału.
Krótkie lub nieregularne transmisje mogą zostać pominięte. Nazwy stacji nie są rozpoznawane.

Widoczna tabela zaczyna się pusta i jest czyszczona przy rozpoczęciu kolejnego skanu;
pokazuje do 2000 ostatnich wyników tylko bieżącego skanowania. Wyniki są nadal dopisywane
do `~/.local/share/SDRTouch/SDRTouch/sygnaly.csv` jako czas UTC, częstotliwość w Hz,
modulacja i poziom; historia w pliku nie jest skracana ani wczytywana do tabeli.
Wybierz lub utwórz nazwaną listę, zaznacz wyniki
i naciśnij **Zapisz zaznaczone**. Przyciski **Zaznacz wszystkie** i **Odznacz wszystkie**
zmieniają zaznaczenie widocznych wyników. Do listy można też ręcznie dodać częstotliwość, modulację
i opcjonalną nazwę albo usunąć zaznaczoną pozycję. Powtórzona częstotliwość z tą samą
modulacją nie jest dodawana ponownie. Usunięcie listy wymaga potwierdzenia.
Listy są zapisywane lokalnie w `~/.local/share/SDRTouch/SDRTouch/listy-czestotliwosci.json`;
istniejący plik CSV pozostaje osobną historią wyników, nie jest automatycznie listą.
Przygotowano listy **Cyfrowe FT8**, **Cyfrowe FT4** i **Cyfrowe WSPR** z częstotliwościami
dial USB dla popularnych pasm, zgodnie z domyślną tabelą WSJT-X. Listy służą do ręcznego
przełączania; po wybraniu pozycji ustaw odpowiedni **Dekoder**. Skaner nie dekoduje
automatycznie kolejnych częstotliwości. Niektóre pasma zależą od anteny i propagacji.

## FT8, FT4 i WSPR

Ustaw częstotliwość dial odpowiednią dla pasma i emisji, np. 14,074 MHz dla FT8,
14,080 MHz dla FT4 albo 14,0956 MHz dla WSPR w paśmie 20 m. Wybierz emisję w polu
**Dekoder**. Program automatycznie używa szerokiego filtra USB; po wyłączeniu dekodera
przywraca poprzedni tryb. Skanowanie i dekodowanie wzajemnie się wykluczają.

W polu **Mój lokator** wpisz własny lokator Maidenhead (4, 6 lub 8 znaków) i naciśnij
**Zapisz QTH**. Jest zapisywany lokalnie. Puste pole usuwa zapisany lokator.
W zakładce **Wiadomości cyfrowe** widać odczytane wiadomości, znak, flagę i polską nazwę
kraju lub obszaru oraz odległość. Odległość jest przybliżona: liczona między środkami
lokatorów i dostępna tylko, gdy wiadomość zawiera lokator stacji. Flaga i nazwa wynikają
z prefiksu znaku, nie są potwierdzeniem fizycznego położenia. Nierozpoznane wartości
oznaczono `—`. Tabela wiadomości nie jest zapisywana po zamknięciu programu.
Przyciski **Najnowsze** i **Najdalsze** ustawiają sortowanie automatycznie utrzymywane
po nadejściu kolejnych wiadomości; domyślnie najnowsze dekodowania są na górze.
Odległość wymaga obu lokatorów, a wiadomości bez niej są na końcu również przy wyborze
**Najdalsze**. Dotknięcie nagłówka kolumny sortuje wyniki; kolejne zmienia kierunek.
Odległości, poziomy SNR i częstotliwości sortowane są liczbowo. **Wyczyść wyniki**
po potwierdzeniu usuwa wiadomości cyfrowe z bieżącej sesji, bez usuwania list
częstotliwości ani historii skanowania. Nowo odebrane wiadomości pojawią się ponownie.

Wskaźnik **NTP: OK** informuje, że czas systemowy jest zsynchronizowany; poprawny czas
jest ważny dla dekodowania. Program zbiera pełny przedział UTC przed dekodowaniem:
15 s dla FT8, 7,5 s dla FT4 i 120 s dla WSPR. Po włączeniu może najpierw czekać na
początek następnego przedziału. Wyniki zależą od anteny, propagacji, poprawnej
częstotliwości i czasu, a także opóźnienia toru odbiorczego. Dekodowanie odbywa się
lokalnie przez programy WSJT-X; aplikacja nie wysyła wyników do sieci. Mapy jeszcze nie ma.

## DMR i C4FM (Yaesu System Fusion)

Na ekranie **Odbiornik** ustaw częstotliwość i wybierz **DMR** lub **C4FM (YSF)**
w polu **Dekoder**. Program automatycznie używa toru NFM odpowiedniego dla danych
cyfrowych; po wyłączeniu dekodera przywraca poprzedni tryb. Odtwarzana jest tylko
nieszyfrowana mowa; **Wycisz** nie wyłącza dekodowania. Jeśli dane są dostępne, pod
wyborem dekodera pojawia się color code, slot, identyfikator celu i źródła DMR albo
znak źródłowy YSF. Są to ostatnie poprawnie odebrane dane, nie pełna historia.
W DMR sloty są rozdzielone na lewy i prawy kanał audio.

Częstotliwości DMR/YSF można zapisać na liście jako **NFM**, a następnie ręcznie wybrać
odpowiedni dekoder. Skaner nie rozpoznaje protokołu. YSF jest jednym z systemów
używających modulacji C4FM, nie oznacza obsługi wszystkich takich systemów.
Odbiór zależy od sygnału, anteny i właściwej częstotliwości. Nie ma obsługi trunkingu,
kluczy, deszyfrowania ani nadawania. Skanowanie, ADS-B i dekodery współdzielą jeden tuner.
Dekodowanie sprawdzono na próbkach testowych, ale odbiór rzeczywistej rozmowy DMR/YSF
nie został jeszcze potwierdzony. Na SR5WU (438,775 MHz) obserwowano brak danych;
przyczyna wymaga dalszej diagnostyki.

Dekodowanie odbywa się lokalnie przez `~/sdr-touch-dsd-fme/build/dsd-fme` i MBElib.
MBElib ostrzega, że kodeki głosu mogą być objęte patentami; przed budową i użyciem
należy sprawdzić wymagania licencyjne i ograniczenia właściwe dla swojego kraju:
https://github.com/lwvmobile/mbelib#readme. DSD-FME i MBElib zawierają własne noty
licencyjne; zachowaj ich katalogi źródłowe. Żadne klucze nie są przekazywane dekoderowi.
Podczas kompilacji DSD-FME występują ostrzeżenia w kodzie obsługi kluczy, którego
aplikacja nie włącza; nie jest to pełny audyt bezpieczeństwa dekodera.

Przykładowa budowa zależności w katalogu użytkownika (wymaga kompilatora, CMake,
`libsndfile1-dev`, `libpulse-dev` i `libncurses-dev`):

```sh
git clone --branch ambe_tones https://github.com/lwvmobile/mbelib.git ~/sdr-touch-mbelib
git -C ~/sdr-touch-mbelib checkout 30dc79074ca022366a27d705b8023011d9600339
cmake -S ~/sdr-touch-mbelib -B ~/sdr-touch-mbelib/build -DCMAKE_BUILD_TYPE=Release
cmake --build ~/sdr-touch-mbelib/build -j 4
git clone --branch audio_work https://github.com/lwvmobile/dsd-fme.git ~/sdr-touch-dsd-fme
git -C ~/sdr-touch-dsd-fme checkout 34753963b8855c6c07b79250cb4de4f2c38a4613
cmake -S ~/sdr-touch-dsd-fme -B ~/sdr-touch-dsd-fme/build -DCMAKE_BUILD_TYPE=Release \
  -DMBE_LIBRARY="$HOME/sdr-touch-mbelib/build/libmbe.a" -DMBE_INCLUDE_DIR="$HOME/sdr-touch-mbelib"
cmake --build ~/sdr-touch-dsd-fme/build -j 4
```

## Samoloty ADS-B

W zakładce **Samoloty ADS-B** wpisz szerokość i długość geograficzną w stopniach
dziesiętnych z kropką, np. `52.123456` i `21.123456`, i naciśnij **Zapisz pozycję**.
Na południe i zachód użyj wartości ujemnych. Współrzędne są zapisywane lokalnie,
oddzielnie od lokatora Maidenhead; wyczyszczenie obu pól i zapis usuwa tę pozycję.
Nie są konieczne do odbioru, ale bez nich odległość nie jest dostępna.

Naciśnij **Uruchom ADS-B**. Odbiornik przestraja się na 1090 MHz, a normalny odsłuch,
skanowanie i dekoder FT są wstrzymane, ponieważ RSP1 ma jeden tuner. **Zatrzymaj ADS-B**
uruchamia ponownie zwykły odbiór i poprzedni tryb cyfrowy. Tabela pokazuje identyfikator,
znak lotu, wysokość w metrach, prędkość w km/h, kurs w stopniach, odległość w km,
pozycję i wiek ostatniej wiadomości w sekundach. Nagłówki sortują tabelę; brakujące
wartości oznacza `—`. Pozycje starsze niż 60 sekund nie są wyświetlane. Odległość
jest przybliżona i liczona po powierzchni Ziemi, a nie w przestrzeni.

Nie każda wiadomość Mode S zawiera pozycję lub inne dane, więc samolot może być widoczny
bez odległości. Potrzebna jest antena odpowiednia na 1090 MHz i możliwie dobry widok
nieba. Odbiór zależy od anteny, otoczenia i ruchu lotniczego. Dane nie służą do nawigacji.
Program nie udostępnia mapy ani nie wysyła danych do sieci.

Aplikacja uruchamia lokalny `~/sdr-touch-readsb/readsb` z obsługą SoapySDR; pakiet
systemowy `readsb` nie musi jej zawierać. Na tej instalacji użyto kodu
https://github.com/wiedehopf/readsb w wersji `88877b96dff8f82cf75a7759f67dccabc85edcfb`,
zbudowanego poleceniem `make -j4 SOAPYSDR=yes DISABLE_INTERACTIVE=yes` w katalogu
źródeł. Wymaga kompilatora, zależności projektu readsb oraz bibliotek i modułu
SoapySDR dla SDRplay. Katalog zawiera również licencję i kod źródłowy dekodera;
nie instalowano usługi systemowej.

## Budowanie i testy

```sh
cmake -S ~/sdr-touch -B ~/sdr-touch/build -DCMAKE_BUILD_TYPE=Release
cmake --build ~/sdr-touch/build -j 4
ctest --test-dir ~/sdr-touch/build --output-on-failure
```

Testy CTest sprawdzają syntetyczne sygnały i zapis skanera. Testy sprzętowe wymagają wolnego RSP1:

```sh
QT_QPA_PLATFORM=offscreen ~/sdr-touch/build/sdr-touch --hardware-test
QT_QPA_PLATFORM=offscreen ~/sdr-touch/build/sdr-touch --scan-test
```

Test skanera używa katalogu tymczasowego i dopuszcza brak wykrytych sygnałów; jego trafienia
nie są potwierdzeniem odbioru stacji. Poprawność odsłuchu zależy też od anteny i konfiguracji audio.
