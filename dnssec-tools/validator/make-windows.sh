export PATH=/c/QtSDK/mingw/bin/:$PATH
#./configure --enable-static --disable-shared --with-openssl=/c/MinGW/msys/1.0/
#
# do first:
#    cp /c/QtSDK/Desktop/Qt/4.8.1/mingw/{bin/libeay32.dll,lib/}
#    cp -pr /c/MinGW/msys/1.0/include/openssl /c/QtSDK/Desktop/Qt/4.8.1/mingw/include/
#
./configure --enable-static --disable-shared --with-openssl=/c/QtSDK/Desktop/Qt/4.8.1/mingw/
