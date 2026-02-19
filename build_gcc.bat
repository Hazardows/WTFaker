mkdir WTFaker
xcopy "como usar.txt" "WTFaker" /y
call g++ -std=c++11 WTFaker.cpp -static -lpsapi -o WTFaker/WTFaker.exe