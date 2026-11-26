// zodiac::platform::append_argument: appends one argument of a Windows command line, quoted so that CommandLineToArgvW (and the
// UCRT start-up code, which agrees on every line this writes) gives back exactly that argument. Header-only and free of system calls,
// so the tests check it on every platform; run_process and restart_elevated build their command lines with it, in UTF-16 there.
// The rules it inverts, for an argument after the program name:
// - outside quotes, a space or a tab ends the argument; a quote opens a quoted part, in which spaces and tabs are kept;
// - 2n backslashes followed by a quote are n backslashes, and the quote opens or closes a quoted part;
// - 2n+1 backslashes followed by a quote are n backslashes and a literal quote;
// - backslashes not followed by a quote are literal.
// A non-empty argument with no space, tab, newline, vertical tab or quote is appended as it is (backslashes in it are literal). Any other
// is put in quotes: each quote is escaped with a backslash, and the run of backslashes in front of a quote, or of the closing quote, is
// doubled. The empty argument is "". The caller puts one space between arguments and reserves the line (at most 2 * size + 3 per
// argument with its space), so the appends never reallocate.
#pragma once
#include <cstddef>
#include <string>
#include <string_view>

namespace zodiac::platform {
template<class Char>
void append_argument(std::basic_string<Char>& line,std::basic_string_view<Char> argument)
{
constexpr Char quote=Char('"');
constexpr Char backslash=Char('\\');
bool plain=!argument.empty();
for(const Char c:argument){
if(c==Char(' ')||c==Char('\t')||c==Char('\n')||c==Char('\v')||c==quote){
plain=false;
break;
}
}
if(plain){
line.append(argument);
return;
}
line.push_back(quote);
// argument[start, i) is still to be appended; backslashes counts the run of backslashes just before i.
std::size_t start=0;
std::size_t backslashes=0;
for(std::size_t i=0;i<argument.size();++i){
const Char c=argument[i];
if(c==backslash){
++backslashes;
continue;
}
if(c==quote){
// The pending text ends with the run once; the run again and one more backslash escape the quote.
line.append(argument.substr(start,i-start));
line.append(backslashes+1,backslash);
line.push_back(quote);
start=i+1;
}
backslashes=0;
}
line.append(argument.substr(start));
// The closing quote must not be escaped by the backslashes in front of it.
line.append(backslashes,backslash);
line.push_back(quote);
}
// The room append_argument may need for argument, plus one separating space.
constexpr std::size_t argument_room(std::size_t size) noexcept
{
return 2*size+3;
}
}
