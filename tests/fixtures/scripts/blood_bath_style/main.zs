// A game laid out the way BGT games such as Blood Bath are: its data and sounds are loaded by literal paths, which a release
// reads from the pack zodiac build appends to the executable (the identity test compares what it prints with zodiac run's).
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
return 7;
}
