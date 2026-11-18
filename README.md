# Operating Systems Custom Shell Project

## Building filecrypt

### Linux

Install the required packages:

```bash
sudo apt update
sudo apt install build-essential libssl-dev pkg-config
```

### macOS

Install the required packages:

```bash
brew install openssl@3 pkg-config
```

### Test filecrypt

The test script compiles filecrypt and runs its test cases:

```bash
./tests/filecrypt/test_filecrypt.sh

``` 

### Run filecrypt

Encrypt a file:

```bash
./source/filecrypt/filecrypt \
    -e \
    -i tests/filecrypt/test_input.txt \
    -o encrypted.fc
```

Decrypt a file:

```bash
./source/filecrypt/filecrypt \
    -d \
    -i encrypted.fc \
    -o recovered.txt
```



## Custom shell integration

The group leader will compile your source with the combined shell:

```bash
cc \
    -DFILECRYPT_NO_MAIN \
    source/main.c \
    source/filecrypt/filecrypt.c \
    source/filecrypt/crypto.c \
    -o customshell \
    $(pkg-config --cflags --libs openssl)
```
