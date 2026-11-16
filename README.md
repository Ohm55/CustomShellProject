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

### Compile filecrypt

From the root of the repository:

```bash
make -C source/filecrypt
```

### Run the tests

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