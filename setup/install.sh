cd ../bd
make clean
cd ../setup
./get_module.symvers.sh
cd ../bd
make
sudo make install

cd ../daemon
make clean
make
cd ../setup
