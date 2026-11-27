// A game laid out the way BGT games such as Blood Bath are: its data and sounds are loaded by literal paths, which a release
// reads from the pack zodiac build appends to the executable, from <game>.zpk beside it, or from plain files with [pack] enabled =
// false. It uses the string, file, timer, random, date and dictionary operations of core-types spec 9 and prints every result with
// example_print, so the identity tests (tests/e2e/test_build.py) compare a release's output with zodiac run's in every pack mode.
// What depends on the moment (the timer) is printed only as a fact that holds every time; the save goes to data:/ and is deleted
// again.
int main()
{
string map=read_file("data/haritalar/ölüm_vadisi.txt");
example_print(map);
array<string>@ sounds=list_files("sounds");
for(int i=0;i<sounds.length();i++)
{
example_print(sounds[i]+" "+file_size("sounds/"+sounds[i]));
}
if(!file_exists("sounds/silah_ateşi.ogg"))return 3;
example_print(file_modified("data/haritalar/ölüm_vadisi.txt").to_string("{yyyy}-{MM}-{dd} {HH}:{mm}:{ss}"));
file settings;
if(!settings.open("data/ayarlar.ini","r"))return 4;
example_print(settings.read_line());
example_print(settings.read_line());
settings.close();
// The string operations a BGT game parses its data with.
array<string>@ halves=map.split(": ");
example_print(halves.length()+" | "+halves[0]+" | "+halves[1]);
example_print(map.replace("düşman","zombi"));
example_print(map.left(4));
example_print(map.contains("sağlık")+" "+map.contains("cephane"));
array<string>@ words=halves[1].split(" ");
example_print("düşman x2: "+(words[0].to_number()*2));
// A map read line by line through a file object, its cells parsed as numbers.
file level;
if(!level.open("data/haritalar/kuzey_ormanı.txt","r"))return 5;
double total=0;
while(!level.eof)
{
string line=level.read_line();
if(line=="")continue;
array<string>@ cells=line.split(" ");
example_print(cells[0]+" @ "+cells[1]+","+cells[2]);
total+=cells[1].to_number()+cells[2].to_number();
}
level.close();
example_print("toplam "+total);
// A save in data:/: made, read back and deleted.
example_print("kayıt var mı: "+file_exists("data:/kayıtlar/oyun.sav"));
example_print("klasör: "+create_directory("data:/kayıtlar"));
example_print("klasör var mı: "+directory_exists("data:/kayıtlar"));
if(!write_file("data:/kayıtlar/oyun.sav","seviye=3\ncan=87\n"))return 6;
example_print("kayıt var mı: "+file_exists("data:/kayıtlar/oyun.sav"));
example_print(read_file("data:/kayıtlar/oyun.sav").replace("\n",";"));
example_print("silindi: "+delete_file("data:/kayıtlar/oyun.sav")+" "+file_exists("data:/kayıtlar/oyun.sav"));
example_print("klasör silindi: "+delete_directory("data:/kayıtlar"));
// Time, random numbers, rounding, a date and a dictionary.
timer clock;
example_print("elapsed>=0 "+(clock.elapsed>=0));
random_generator dice(7);
string rolls;
for(int i=0;i<8;i++)rolls+=dice.random(1,6)+" ";
example_print(rolls);
example_print("float "+dice.random_float()+" "+dice.random_bool()+" "+dice.random_float(-1,1));
example_print("round "+round(2.675,2));
example_print(date_time(2026,11,19).to_string());
dictionary player;
player.set("ad","Kanka");
player.set("can",87);
player.set("hız",1.5);
example_print(player.serialize());
return 7;
}
