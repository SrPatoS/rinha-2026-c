#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VECTOR_DIMS 14
#define VECTOR_SCALE 10000.0f

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') {
        p++;
    }
    return p;
}

static int16_t quantize(float value) {
    int scaled = (int)lroundf(value * VECTOR_SCALE);
    if (scaled < -10000) {
        return -10000;
    }
    if (scaled > 10000) {
        return 10000;
    }
    return (int16_t)scaled;
}

static char *read_all(FILE *file, size_t *size_out) {
    size_t capacity = 1024 * 1024;
    size_t size = 0;
    char *data = malloc(capacity + 1);
    if (!data) {
        return NULL;
    }

    for (;;) {
        if (size == capacity) {
            capacity *= 2;
            char *next = realloc(data, capacity + 1);
            if (!next) {
                free(data);
                return NULL;
            }
            data = next;
        }
        size_t read = fread(data + size, 1, capacity - size, file);
        size += read;
        if (read == 0) {
            if (ferror(file)) {
                free(data);
                return NULL;
            }
            break;
        }
    }

    data[size] = '\0';
    *size_out = size;
    return data;
}

static uint32_t count_vectors(const char *data) {
    uint32_t count = 0;
    const char *p = data;
    while ((p = strstr(p, "\"vector\"")) != NULL) {
        count++;
        p += 8;
    }
    return count;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s input.references.json output.references.bin\n", argv[0]);
        return 2;
    }

    FILE *input = fopen(argv[1], "rb");
    if (!input) {
        perror("open input");
        return 1;
    }

    size_t size = 0;
    char *data = read_all(input, &size);
    fclose(input);
    if (!data || size == 0) {
        fprintf(stderr, "failed to read input\n");
        free(data);
        return 1;
    }

    uint32_t count = count_vectors(data);
    FILE *output = fopen(argv[2], "wb");
    if (!output) {
        perror("open output");
        free(data);
        return 1;
    }

    fwrite("R26B", 1, 4, output);
    fwrite(&count, sizeof(count), 1, output);

    uint32_t written = 0;
    const char *p = data;
    while ((p = strstr(p, "\"vector\"")) != NULL) {
        int16_t vector[VECTOR_DIMS];
        p = strchr(p, '[');
        if (!p) {
            break;
        }
        p++;

        for (int i = 0; i < VECTOR_DIMS; i++) {
            p = skip_ws(p);
            char *end = NULL;
            vector[i] = quantize(strtof(p, &end));
            if (end == p) {
                fprintf(stderr, "invalid vector at record %u dim %d\n", written, i);
                fclose(output);
                free(data);
                return 1;
            }
            p = end;
            while (*p && *p != ',' && *p != ']') {
                p++;
            }
            if (*p == ',') {
                p++;
            }
        }

        const char *label = strstr(p, "\"label\"");
        const char *object_end = strchr(p, '}');
        if (!label || !object_end) {
            fprintf(stderr, "missing label at record %u\n", written);
            fclose(output);
            free(data);
            return 1;
        }

        uint8_t fraud = strstr(label, "\"fraud\"") && strstr(label, "\"fraud\"") < object_end;
        fwrite(vector, sizeof(int16_t), VECTOR_DIMS, output);
        fwrite(&fraud, sizeof(uint8_t), 1, output);
        written++;
        p = object_end + 1;
    }

    fclose(output);
    free(data);

    fprintf(stderr, "converted %u references\n", written);
    return written == count ? 0 : 1;
}
