CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -D_GNU_SOURCE
LDFLAGS = -lpthread -lm
TARGET  = fms

all: $(TARGET)

$(TARGET): fms.c
	$(CC) $(CFLAGS) -o $(TARGET) fms.c $(LDFLAGS)
	@echo "Compilado com sucesso: ./$(TARGET)"

clean:
	rm -f $(TARGET) stress_test

# Programa de teste (consome CPU e memória)
stress_test: stress_test.c
	$(CC) $(CFLAGS) -o stress_test stress_test.c -lm

.PHONY: all clean
