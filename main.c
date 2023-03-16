#define _GNU_SOURCE 
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>

#include "jpeg-6b-steg/jpeglib.h"

static int detect_quality_table(JQUANT_TBL* table, const unsigned int* basic_table) {
  for (int quality = 1; quality <= 100; quality++) {
    int percentage_scaling = quality < 50 ? 5000 / quality : 200 - quality*2;

    bool found_quality = true;
    for (int i = 0; i < DCTSIZE2; i++) {
      long expected_value = (basic_table[i] * percentage_scaling + 50) / 100;

      // Let's force baseline compatibility, as that's the default settings of JPEG 6b
      if (expected_value <= 0) expected_value = 1;
      if (expected_value > 255) expected_value = 255;
      
      if (table->quantval[i] != expected_value) {
        found_quality = false;
        break;
      }
    }

    if (found_quality)
      return quality;
  }

  return -1;
}

static int detect_quality(struct jpeg_decompress_struct* cinfo) {
  int quality = -1;

  // Luminance
  JQUANT_TBL* table = cinfo->quant_tbl_ptrs[0];
  if (table == NULL) {
    puts("Luminance table missing.");
    return -1;
  }

  static const unsigned int std_luminance_quant_tbl[DCTSIZE2] = {
    16,  11,  10,  16,  24,  40,  51,  61,
    12,  12,  14,  19,  26,  58,  60,  55,
    14,  13,  16,  24,  40,  57,  69,  56,
    14,  17,  22,  29,  51,  87,  80,  62,
    18,  22,  37,  56,  68, 109, 103,  77,
    24,  35,  55,  64,  81, 104, 113,  92,
    49,  64,  78,  87, 103, 121, 120, 101,
    72,  92,  95,  98, 112, 100, 103,  99
  };

  quality = detect_quality_table(table, std_luminance_quant_tbl);
  if (quality == -1) {
    puts("Could not detect quality of the luminance table.");
    return -1;
  }

  // Chrominance
  table = cinfo->quant_tbl_ptrs[1];
  if (table == NULL) {
    puts("Chrominance table missing.");
    return -1;
  }

  static const unsigned int std_chrominance_quant_tbl[DCTSIZE2] = {
    17,  18,  24,  47,  99,  99,  99,  99,
    18,  21,  26,  66,  99,  99,  99,  99,
    24,  26,  56,  99,  99,  99,  99,  99,
    47,  66,  99,  99,  99,  99,  99,  99,
    99,  99,  99,  99,  99,  99,  99,  99,
    99,  99,  99,  99,  99,  99,  99,  99,
    99,  99,  99,  99,  99,  99,  99,  99,
    99,  99,  99,  99,  99,  99,  99,  99
  };

  int table_quality = detect_quality_table(table, std_chrominance_quant_tbl);
  if (table_quality != quality) {
    puts("Could not detect quality of the chrominance table.");
    return -1;
  }

  // Any other table?
  if (cinfo->quant_tbl_ptrs[2] != NULL) {
    puts("Extra quantization table, can't detect the quality of these.");
    return -1;
  }

  return quality;  
}

static void on_error_exit(j_common_ptr cinfo) {
	char buffer[JMSG_LENGTH_MAX];

	(*cinfo->err->format_message)(cinfo, buffer);

	jpeg_destroy(cinfo);

  puts(buffer);
}
static void on_output_message(j_common_ptr cinfo) {
	char buffer[JMSG_LENGTH_MAX];

	(*cinfo->err->format_message)(cinfo, buffer);

	puts(buffer);
}

bool check_image_metadata(char* filename) {
  FILE* input = fopen(filename, "r");
  if (input == NULL) {
    printf("Cannot open %s\n", filename);
    exit(1);
  }

  // Input image parameters
  int width, height;
  int quality_factor;
  jvirt_barray_ptr* coeffs;

  // Reading input image.
  struct jpeg_decompress_struct in_cinfo;
  struct jpeg_error_mgr jerr;

  in_cinfo.err = jpeg_std_error(&jerr);
  in_cinfo.err->error_exit = on_error_exit; 
  in_cinfo.err->output_message = on_output_message;
  jpeg_create_decompress(&in_cinfo);

  jpeg_stdio_src(&in_cinfo, input);

  (void)jpeg_read_header(&in_cinfo, TRUE);

  jpeg_calc_output_dimensions(&in_cinfo);

  width = in_cinfo.output_width;
  height = in_cinfo.output_height;

  coeffs = jpeg_read_coefficients(&in_cinfo);

  // Detecting quality
  quality_factor = detect_quality(&in_cinfo);

  // Attempt to reproduce JPEG file
  char* output_filename;
  time_t rawtime;
  time(&rawtime);
  if (asprintf(&output_filename, "/tmp/%ld.jpg", rawtime) == -1) {
    puts("Out-of-memory!");
    abort();
  }

  FILE* output = fopen(output_filename, "w+b");

  struct jpeg_compress_struct out_cinfo;

  out_cinfo.err = jpeg_std_error(&jerr);
  out_cinfo.err->error_exit = on_error_exit;
  out_cinfo.err->output_message = on_output_message;
  jpeg_create_compress(&out_cinfo);
  
  jpeg_stdio_dest(&out_cinfo, output);
  
  out_cinfo.image_width = width;
  out_cinfo.image_height = height;
  out_cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&out_cinfo);

  jpeg_set_quality(&out_cinfo, quality_factor, TRUE);

  jpeg_write_coefficients(&out_cinfo, coeffs);
  (void)jpeg_finish_compress(&out_cinfo);

  // Comparing the two files 
  rewind(input);
  rewind(output);

  bool matching_metadata = true;

  int c1, c2;
  do {
    c1 = fgetc(input);
    c2 = fgetc(output);
  } while (c1 != EOF && c2 != EOF && c1 == c2);  

  matching_metadata = c1 == EOF && c2 == EOF;

  // Cleanup
  jpeg_destroy_compress(&out_cinfo);

  fclose(output);
  remove(output_filename);
  free(output_filename);

  (void)jpeg_finish_decompress(&in_cinfo);
  jpeg_destroy_decompress(&in_cinfo);

  fclose(input);

  return matching_metadata;
}

static jvirt_barray_ptr* read_DCT_coefficients(struct jpeg_decompress_struct* cinfo, FILE* image) {
  static struct jpeg_error_mgr jerr;

  cinfo->err = jpeg_std_error(&jerr);
  cinfo->err->error_exit = on_error_exit;
  cinfo->err->output_message = on_output_message;
  jpeg_create_decompress(cinfo);

  jpeg_stdio_src(cinfo, image);

  (void)jpeg_read_header(cinfo, TRUE);

  jpeg_calc_output_dimensions(cinfo);

  return jpeg_read_coefficients(cinfo);
}

int min(int a, int b) {
  return a < b ? a : b;
}

bool check_invariant(char* filename_1, char* filename_2) {
  FILE* input_1 = fopen(filename_1, "rb");
  if (input_1 == NULL) {
    printf("Cannot open '%s'.\n", filename_1);
    exit(1);
  }
  FILE* input_2 = fopen(filename_2, "rb");
  if (input_2 == NULL) {
    printf("Cannot open '%s'.\n", filename_2);
    exit(1);
  }

  /* Reading input images. */
  struct jpeg_decompress_struct in1_cinfo;
  struct jpeg_decompress_struct in2_cinfo;
  jvirt_barray_ptr* coeffs_1 = read_DCT_coefficients(&in1_cinfo, input_1);
  jvirt_barray_ptr* coeffs_2 = read_DCT_coefficients(&in2_cinfo, input_2);

  // Images produced by Outguess have an known DCT "structure"
  if (in1_cinfo.num_components != 3 || in1_cinfo.jpeg_color_space != JCS_YCbCr)
    return false;
  if (in2_cinfo.num_components != 3 || in2_cinfo.jpeg_color_space != JCS_YCbCr)
    return false;

  jpeg_component_info* components_info_1 = in1_cinfo.comp_info;
  jpeg_component_info* components_info_2 = in2_cinfo.comp_info;
  if (components_info_1[0].h_samp_factor != 2 || components_info_1[0].v_samp_factor != 2 ||
      components_info_1[1].h_samp_factor != 1 || components_info_1[1].v_samp_factor != 1 ||
      components_info_1[2].h_samp_factor != 1 || components_info_1[2].v_samp_factor != 1)
    return false;
  if (components_info_1[0].h_samp_factor != 2 || components_info_1[0].v_samp_factor != 2 ||
      components_info_1[1].h_samp_factor != 1 || components_info_1[1].v_samp_factor != 1 ||
      components_info_1[2].h_samp_factor != 1 || components_info_1[2].v_samp_factor != 1)
    return false;

  /* Reading the coefficients */
  if (components_info_1[0].width_in_blocks != components_info_2[0].width_in_blocks ||
      components_info_1[0].height_in_blocks != components_info_2[0].height_in_blocks ||
      components_info_1[1].width_in_blocks != components_info_2[1].width_in_blocks ||
      components_info_1[1].height_in_blocks != components_info_2[1].height_in_blocks ||
      components_info_1[2].width_in_blocks != components_info_2[2].width_in_blocks ||
      components_info_1[2].height_in_blocks != components_info_2[2].height_in_blocks)
    return false;
  if (components_info_1[0].h_samp_factor != components_info_2[0].h_samp_factor ||
      components_info_1[0].v_samp_factor != components_info_2[0].v_samp_factor ||
      components_info_1[1].h_samp_factor != components_info_2[1].h_samp_factor ||
      components_info_1[1].v_samp_factor != components_info_2[1].v_samp_factor ||
      components_info_1[2].h_samp_factor != components_info_2[2].h_samp_factor ||
      components_info_1[2].v_samp_factor != components_info_2[2].v_samp_factor)
    return false;

  int width_blocks = components_info_1[1].width_in_blocks;
  int height_blocks = components_info_1[1].height_in_blocks;

  for (int by = 0; by < height_blocks; by++) {
    for (int bx = 0; bx < width_blocks; bx++) {
      jvirt_barray_ptr component_coeffs_1, component_coeffs_2;
      JBLOCKROW row_coeffs_1, row_coeffs_2;
      JCOEFPTR block_coeffs_1, block_coeffs_2;

      // Coefficient order: "natural" order
      // https://en.wikipedia.org/wiki/JPEG#/media/File:Dctjpeg.png
      //    0  1  2  3  4  5  6  7
      //    8  9 10 11 12 13 14 15
      //   16 17 18 19 20 21 22 23
      //   24 25 26 27 28 29 30 31
      //   32 33 34 35 36 37 38 39
      //   40 41 42 44 43 45 46 47
      //   48 49 50 51 52 53 54 55
      //   56 57 58 59 60 61 62 63

      for (int plane = 0; plane < 3; plane++) {
        int width_blocks = components_info_1[plane].width_in_blocks;
        int height_blocks = components_info_1[plane].height_in_blocks;
  
        int h_sampling_vector = components_info_1[plane].h_samp_factor;
        int v_sampling_vector = components_info_1[plane].v_samp_factor;
  
        for (int sub_by = v_sampling_vector*by; sub_by < min(height_blocks, v_sampling_vector*(by + 1)); sub_by++) {
          for (int sub_bx = h_sampling_vector*bx; sub_bx < min(width_blocks, h_sampling_vector*(bx + 1)); sub_bx++) {
            component_coeffs_1 = coeffs_1[plane];
            row_coeffs_1 = in1_cinfo.mem->access_virt_barray((j_common_ptr)&in1_cinfo, component_coeffs_1, sub_by, 1, TRUE)[0];
            block_coeffs_1 = row_coeffs_1[sub_bx];

            component_coeffs_2 = coeffs_2[plane];
            row_coeffs_2 = in2_cinfo.mem->access_virt_barray((j_common_ptr)&in2_cinfo, component_coeffs_2, sub_by, 1, TRUE)[0];
            block_coeffs_2 = row_coeffs_2[sub_bx];
  
            unsigned histogram_1[256] = {0};
            unsigned histogram_2[256] = {0};

            for (int i = 0; i < DCTSIZE2; i++) {
              histogram_1[128 + (block_coeffs_1[i] >> 1)]++;
              histogram_2[128 + (block_coeffs_2[i] >> 1)]++;
            }

            for (int i = 0; i < 256; i++) {
              if (histogram_1[i] != histogram_2[i])
                return false;
            }
          }
        }
      }
    }
  }

  return true;
}

int main(int argc, char** argv) {
  if (argc != 3) {
    printf("Usage: %s <image 1.jpg> <image 2.jpg>\n", argv[0]);
    return 0;
  }

  // First checks
  bool matching_metadata_1 = check_image_metadata(argv[1]);
  bool matching_metadata_2 = check_image_metadata(argv[2]);

  if (!matching_metadata_1) {
    printf("%s can't possibly have been generated by Outguess (metadata mismatch)\n", argv[1]);
    return 1;
  }
  if (!matching_metadata_2) {
    printf("%s can't possibly have been generated by Outguess (metadata mismatch)\n", argv[2]);
    return 1;
  }

  // Second check
  bool pairs_invariant_respected = check_invariant(argv[1], argv[2]);

  if (!pairs_invariant_respected) {
    printf("%s and %s can't possibly have been generated by Outguess from the same source image (invariant not respected)\n", argv[1], argv[2]);
    return 1;
  }

  printf("It is likely that %s and %s have been produced by Outguess from the same source image\n", argv[1], argv[2]); 
  return 0;
}
