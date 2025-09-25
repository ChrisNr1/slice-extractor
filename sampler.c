#include <stdio.h>
#include <tiffio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <grok/grok_codec.h>
#include <immintrin.h>


typedef struct {
    uint8_t* tile;
    uint16_t tilelength;
    char first_dim;
} Tile;

typedef struct {
    uint64_t x;
    uint64_t y;
    const char* fname;
    float tolerance;
    uint16_t tile_size;
    uint8_t magnification;
} load_tile_args;


void free_tile(Tile* tile) {
    if(tile) {
        free(tile->tile);
        free(tile);
    }
}

void free_tiles(Tile** tiles, uint16_t n_tiles) {
    if (tiles) {
        for (uint16_t i = 0; i < n_tiles; i++) {
            free_tile(tiles[i]);
        }
        free(tiles);
    }
}

/*
Changes the tif page to the most fitting one and returns its downsample ratio.
*/
float get_best_downsample(TIFF* tif, float downsample, float tolerance) {
    if (downsample < 1.) {
        return 1.; // Downsample ratio smaller than 0 page, we need to usample
    }

    uint32_t page_0_img_width;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &page_0_img_width);

    tdir_t last_page_no = 0;
    float closest_downsample = 0;
    do {
        if (!TIFFIsTiled(tif)) // Skip striped pages
            continue;

        uint32_t page_img_width;
        TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &page_img_width);

        float level_downsample = (float)page_0_img_width / page_img_width;

        if (fabs(level_downsample - downsample) <= tolerance)
            return level_downsample; // Exact match
        if (level_downsample > downsample) {
            TIFFSetDirectory(tif, last_page_no); // Set to previous page
            break;
        }
        closest_downsample = level_downsample;
        last_page_no = TIFFCurrentDirectory(tif);
    } while (TIFFReadDirectory(tif));

    if (!TIFFIsTiled(tif)) {
        // Striped page was last page, set back by one
        TIFFSetDirectory(tif, last_page_no);
    }

    return closest_downsample;
}

/*
Check tiff file meta properties. SVS files should have all of the below.
*/
void check_metadata(TIFF* tif) {
    uint16_t tif_metadata;
    TIFFGetField(tif, TIFFTAG_PLANARCONFIG, &tif_metadata);
    if (tif_metadata != PLANARCONFIG_CONTIG) {
        fprintf(stderr, "TIFF File is not contiguous!\n");
        exit(1);
    }
    TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE, &tif_metadata);
    if (tif_metadata != 8) {
        fprintf(stderr, "TIFF File is not in uint8 format. Has: %d bits per sample.\n", tif_metadata);
        exit(1);
    }
    TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &tif_metadata);
    if (tif_metadata != 3) {
        fprintf(stderr, "TIFF File is not in RGB format. Has: %d samples per pixel.\n", tif_metadata);
        exit(1);
    }
}

void* load_tile(void *arg) {
    load_tile_args* args = (load_tile_args*) arg;
    TIFF* tif = TIFFOpen(args->fname, "r");
    if (!tif) {
        fprintf(stderr, "File not found: %s\n", args->fname);
        exit(1);
    }

    char* description;
    char* desc_field;
    TIFFGetField(tif, TIFFTAG_IMAGEDESCRIPTION, &description);
    uint8_t appMag = 0;
    while ((desc_field = strsep((&description), "|"))) {
        if (strncmp(desc_field, "AppMag", 6) == 0) {
            appMag = atoi(strchr(desc_field, '=') + 1);
            break;
        }
    }
    if (!appMag) {
        fprintf(stderr, "AppMag not found in image description.\n");
        TIFFClose(tif);
        exit(1);
    }

    float level_downsample = get_best_downsample(
        tif,
        appMag / (float) args->magnification,
        args->tolerance
    );
    check_metadata(tif);

    // Read relevant tags
    uint32_t page_img_width, tile_width, tile_length;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &page_img_width);
    TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tile_width);
    TIFFGetField(tif, TIFFTAG_TILELENGTH, &tile_length);
    uint16_t img_size = (int)(args->tile_size * appMag / (level_downsample * args->magnification - 0.01)); // substract 0.01 for numerical stability
    uint16_t compression;
    TIFFGetField(tif, TIFFTAG_COMPRESSION, &compression);

    Tile *t = malloc(sizeof(Tile));
    if (!t) {
        perror("Memory allocation failed");
        exit(1);
    }
    t->tilelength = img_size;
    t->tile = malloc(img_size * img_size * 3);
    if (!t->tile) {
        perror("Memory allocation failed");
        free(t);
        exit(1);
    }

    uint16_t crop_left = (int)(args->x / level_downsample) % tile_width; // coordinate crop converted with tile downsample
    uint16_t crop_top = (int)(args->y / level_downsample) % tile_length;
    uint8_t buf[TIFFTileSize(tif)];

    if (TIFFIsCODECConfigured(compression)) {
        t->first_dim = 'H'; // HxWxC format

        uint16_t y_budget = img_size; // Pixel 'budget' that describes the amount of pixels written into the big buffer
        // Horizontal image insertion
        for (uint8_t y_count = 0; y_budget > 0; y_count++) {
            uint16_t x_budget = img_size;
            // Take either the whole tile size or just the edge we still need
            uint16_t row_count = tile_length < y_budget ? tile_length : y_budget;
            if (y_count == 0) {
                row_count = tile_length - crop_top < y_budget ? tile_length - crop_top : y_budget;
            }
            // Vertical image insertion, for each tile at x_count
            for (uint8_t x_count = 0; x_budget > 0; x_count++) {
                // Check compression types before reading
                TIFFReadTile(
                    tif,
                    buf,
                    (int)(args->x / level_downsample) + x_count * tile_width,
                    (int)(args->y / level_downsample) + y_count * tile_length,
                    0,
                    0
                );

                uint32_t offset_dest = (img_size - y_budget) * img_size + (img_size - x_budget);
                uint32_t offset_src = y_count ? 0 : crop_top * tile_width;

                uint16_t x_copy_size = tile_width < x_budget ? tile_width : x_budget;
                if (x_count == 0) {
                    offset_src += crop_left;
                    x_copy_size = tile_width - crop_left < x_budget ? tile_width - crop_left : x_budget;
                }
                for (uint16_t row = 0; row < row_count; row++) {
                    memcpy(
                        t->tile + offset_dest * 3,
                        buf + offset_src * 3,
                        x_copy_size * 3
                    );

                    offset_dest += img_size;
                    offset_src += tile_width;
                }
                x_budget -= x_copy_size; // Reduce bugdet by inserted size
            }
            y_budget -= row_count;
        }
    }

    else { // use grok
        t->first_dim = 'C'; // CxHxW format

        uint16_t y_budget = img_size; // Pixel 'budget' that describes the amount of pixels written into the big buffer
        for (uint8_t y_count = 0; y_budget > 0; y_count++) {
            // for each tile row, reset x_budget
            uint16_t x_budget = img_size;

            uint16_t row_count = tile_length < y_budget ? tile_length : y_budget;
            if (y_count == 0) {
                row_count = tile_length - crop_top < y_budget ? tile_length - crop_top : y_budget;
            }
            for (uint8_t x_count = 0; x_budget > 0; x_count++) {
                TIFFReadRawTile(
                    tif,
                    TIFFComputeTile(
                        tif,
                        (int)(args->x / level_downsample) + x_count * tile_width,
                        (int)(args->y / level_downsample) + y_count * tile_length,
                        0,
                        0
                    ),
                    buf,
                    TIFFTileSize(tif)
                );

                grk_decompress_parameters decompress_params = {
                    .core = {
                        .reduce = 0,
                        .layers_to_decompress = 0,
                    },
                    .compression_level = GRK_DECOMPRESS_COMPRESSION_LEVEL_DEFAULT,
                };

                grk_stream_params stream_params = {
                    .buf = buf,
                    .buf_len = TIFFTileSize(tif)
                };

                grk_object *codec = grk_decompress_init(&stream_params, &decompress_params);
                if(!codec) {
                    fprintf(stderr, "Failed to create grok object for decompression.\n");
                    goto beach;
                }

                grk_header_info headerInfo;
                memset(&headerInfo, 0, sizeof(headerInfo));
                if(!grk_decompress_read_header(codec, &headerInfo)) {
                    fprintf(stderr, "Failed to read the header\n");
                    goto beach;
                }

                grk_image* image = grk_decompress_get_image(codec);
                if (!image || !grk_decompress(codec, NULL)) {
                    fprintf(stderr, "Decompression failed.\n");
                    goto beach;
                }

                uint16_t x_copy_size = tile_width < x_budget ? tile_width : x_budget;
                if (x_count == 0) {
                    x_copy_size = tile_width - crop_left < x_budget ? tile_width - crop_left : x_budget;
                }
                for (uint16_t comp = 0; comp < 3; comp++) {
                    // Calculate boundaries for the tile
                    uint32_t offset_dest = (img_size - y_budget) * img_size + (img_size - x_budget);
                    uint32_t offset_src = y_count ? 0 : crop_top * tile_width;

                    if (x_count == 0) {
                        offset_src += crop_left;
                    }
                    uint32_t tile_offset = comp * img_size * img_size + offset_dest;
                    int32_t* comp_data = image->comps[comp].data;

                    for (uint32_t j = 0; j < row_count; j++) {
                        // Move data to uint8 and then into Tile t using SIMD
                        uint32_t i = 0;
                        for (; i + 64 <= x_copy_size; i += 64) {
                            // Load 64 int32_t values into 4 AVX-512 registers
                            // Use 4 registers in one loop to get performance boost
                            __m512i v0 = _mm512_loadu_si512((__m512i*)&comp_data[offset_src + i]);
                            __m512i v1 = _mm512_loadu_si512((__m512i*)&comp_data[offset_src + i + 16]);
                            __m512i v2 = _mm512_loadu_si512((__m512i*)&comp_data[offset_src + i + 32]);
                            __m512i v3 = _mm512_loadu_si512((__m512i*)&comp_data[offset_src + i + 48]);

                            // Narrow from int32_t -> int16_t
                            __m256i packed0 = _mm512_cvtepi32_epi16(v0);
                            __m256i packed1 = _mm512_cvtepi32_epi16(v1);
                            __m256i packed2 = _mm512_cvtepi32_epi16(v2);
                            __m256i packed3 = _mm512_cvtepi32_epi16(v3);

                            // Narrow from int16_t -> uint8_t
                            __m128i final0 = _mm256_cvtepi16_epi8(packed0);
                            __m128i final1 = _mm256_cvtepi16_epi8(packed1);
                            __m128i final2 = _mm256_cvtepi16_epi8(packed2);
                            __m128i final3 = _mm256_cvtepi16_epi8(packed3);

                            // Store the results with SIMD by treating uint8 values as 128bit
                            _mm_storeu_si128((__m128i*)&t->tile[tile_offset + i], final0);
                            _mm_storeu_si128((__m128i*)&t->tile[tile_offset + i + 16], final1);
                            _mm_storeu_si128((__m128i*)&t->tile[tile_offset + i + 32], final2);
                            _mm_storeu_si128((__m128i*)&t->tile[tile_offset + i + 48], final3);
                        }

                        for (; i < x_copy_size; i++) // Move the rest of the data as well
                            t->tile[tile_offset + i] = (uint8_t)comp_data[offset_src + i];

                        tile_offset += img_size;
                        offset_src += image->comps[comp].stride;
                    }
                }
                grk_object_unref(codec);
                x_budget -= x_copy_size; // Reduce budget by inserted size
            }
            y_budget -= row_count;
        }
    }

    TIFFClose(tif);

    return t;

beach:
    TIFFClose(tif);
    free_tile(t);
    exit(1);
}

Tile* load_single_tile(const char* fname, uint64_t x, uint64_t y, uint16_t tile_size, uint8_t magnification) {
    load_tile_args args = {
        .fname = fname,
        .x = x,
        .y = y,
        .tile_size = tile_size,
        magnification = magnification,
        .tolerance = 0.2
    };
    return (Tile*) load_tile(&args);
}

void init_grok() {
    grk_initialize(NULL, 0);
}

void deref_grok() {
    grk_deinitialize();
}
