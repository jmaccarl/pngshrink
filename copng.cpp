#include <coroutine>
#include <exception>
#include <ios>
#include <iostream>
#include <fstream>
#include <string>
#include <span>
#include <assert.h>

#include "png.h"

// Low memory PNG shrinker, a contrived simple example for learning coroutines,
// inspired by a recent project with image processing in embedded programming
// where I needed to sample pixels from pngs downloaded from an endpoint
//
// Currently reads one image from a file, but a more realistic use of
// this application would be reading image(s) from a websocket,
// which can be Part 2

// libpng boilerplate
void png_err() {
  throw std::runtime_error("There was a libpng issue");
}
#define PNG_ABORT(png_err)
// end libpng boilerplate

// Coroutine task object, can be heap allocated
struct ReturnObj {
  // Used for return types and exceptions
  struct promise_type {
    ~promise_type() {
      std::cout << "promise_type is destroyed" << std::endl;
    }
    ReturnObj get_return_object() {
      return {
        .handle = std::coroutine_handle<promise_type>::from_promise(*this)
      };
    }
    // means to NOT call the coroutine on initialization
    // suspend_never would cause the coroutine to be called
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void unhandled_exception() {
      std::terminate();
    }
    void return_void() {}
  };

  std::coroutine_handle<promise_type> handle;
};


// Awaiter job: needs to read some data, suspend if more needed 
template <size_t bufSize>
class Reader {
 public:
  Reader (std::ifstream && _imageStream) : imageStream(std::move(_imageStream)) {}

  std::ifstream imageStream;
  std::array<std::byte, bufSize> imageBuffer;
  size_t totalRead = 0;
  
  bool await_ready() {
     // will never be true, but worth noting if the stream is full, no need to suspend
     return totalRead == bufSize; 
  }

  bool await_suspend(std::coroutine_handle<> h) {
    size_t numRead = imageStream.readsome((char*)&imageBuffer.at(totalRead),
        imageBuffer.max_size() - totalRead);
    if (numRead == 0) {
        std::cout << "Reached end of file" << std::endl;
        return false; // we are done
    } else if (imageStream.fail()) {
        throw std::runtime_error("There was an error reading the file");
    }
    
    totalRead += numRead;
    assert(totalRead <= bufSize);
    if (totalRead == bufSize) {
        return false; // no need to suspend, we are done
    } else {
        return true; // need to suspend and try again later
    } 
  }

  // the return value here is the return value of co_await
  std::span<std::byte> await_resume() { return {imageBuffer.begin(), totalRead};  }

  void clear() {
    totalRead = 0;
  }
};


namespace PngReadWrite {
  // User-provided struct to be accessed during png processing
  struct userInfo {
    // Check if we are done processing image
    bool isDone = false;
    // Write handle, used for progressive writes
    png_structp png_write_ptr;
    // Parameters for image manipulation
    size_t rowWidth = 0;
    size_t channels = 1;
    unsigned sampleRate = 1;
  };

  void info_callback(png_structp png_ptr, png_infop png_info) {
    std::cout << "Received png info" << std::endl;
    
    // PSA: This MUST be called, even though no transformations are happening
    png_start_read_image(png_ptr);
    
    // Write out the header
    struct userInfo *info = (struct userInfo*)png_get_progressive_ptr(png_ptr);
    
    png_uint_32 width, height;
    int bit_depth, color_type, interlace_type;
    int compression_type, filter_type;
    png_get_IHDR(png_ptr, png_info, &width, &height, &bit_depth,
          &color_type, &interlace_type, &compression_type, &filter_type);
    std::cout << "Image width " << width << " height " << height << std::endl;
    
    png_infop info_write_ptr = png_create_info_struct(info->png_write_ptr);
    if (!info_write_ptr) {
      png_destroy_write_struct(&info->png_write_ptr, (png_infop*)nullptr);
      throw std::runtime_error("Error creating ping write info ptr");
    }

    // Check that the sample rate remotely makes sense
    if (width < info->sampleRate || height < info->sampleRate) {
      throw std::runtime_error("Sample rate outside dimensions of image");
    } 

    // Set up output image header using the shrunk dimensions
    png_set_IHDR(info->png_write_ptr, info_write_ptr, width / info->sampleRate,
        height / info->sampleRate, bit_depth, color_type, interlace_type,
        compression_type, filter_type);
    png_write_info(info->png_write_ptr, info_write_ptr);
    png_write_flush(info->png_write_ptr);

    // We don't need this anymore, destroy it now to reclaim memory
    png_destroy_write_struct(nullptr, &info_write_ptr);
 
    // Get row width and channels for row sampling in later callbacks 
    info->rowWidth = png_get_rowbytes(png_ptr, png_info);
    info->channels = png_get_channels(png_ptr, png_info);
    std::cout << "Row width = " << info->rowWidth << " Num channels = "
        << info->channels << std::endl;
  }

  void row_callback(png_structp png_ptr, png_bytep new_row, png_uint_32 row_num, int pass) {
    // Write out the row
    struct userInfo *info = (struct userInfo*)png_get_progressive_ptr(png_ptr);
    assert(info->rowWidth > 0);

    // Do the image manipulation here - shrink the image using the provided sample
    // rate. Note this doesn't use any fancy algorithms like nearest neighbors,
    // averaging, etc. as its not needed atm, but we could be smarter here 
    if (row_num % info->sampleRate == 0) {
      // Avoid a copy by writing to the same row struct as we shrink the image
      int writePos = 0;
      // Copy in chunks equal to the number of color channels (actual full pixel size)
      for (int i = 0; i < info->rowWidth; i += info->sampleRate*info->channels) {
        // Bounds checking
        if (i + info->channels < info->rowWidth){
          for (int y = 0; y < info->channels; ++y) {
            new_row[writePos+y] = new_row[i+y];
          }
        }
        writePos += info->channels;
      }
      png_write_row(info->png_write_ptr, new_row);
      png_write_flush(info->png_write_ptr);
    }
  }

  void end_callback(png_structp png_ptr, png_infop png_info) {
    std::cout << "Received end of png" << std::endl;
    struct userInfo *info = (struct userInfo*)png_get_progressive_ptr(png_ptr);
    if (info == nullptr) {
      throw std::runtime_error("No info struct in end_callback");
    }
    info->isDone = true;

    // Write out metadata at the end
    png_write_end(info->png_write_ptr, png_info);
    png_write_flush(info->png_write_ptr);
  }
};


ReturnObj coPng(const char* inFilename, const char* outFilename, unsigned sampleRate)
{
  std::ifstream imageStream(inFilename,std::fstream::binary); // fstream:in is implied
  if (!imageStream) {
    throw std::runtime_error("Can't open file to read");
  }
  
  Reader<1024> imageReader{std::move(imageStream)};
  
  // libpng boilerplate here
  //
  // reading setup
  png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL); 
  if (!png_ptr) {
    throw std::runtime_error("Error creating png struct");
  }
  png_infop info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr) {
    png_destroy_read_struct(&png_ptr, (png_infop*)nullptr, (png_infop*)nullptr);
    throw std::runtime_error("Error creating ping info ptr");
  }
  
  // writing setup
  png_structp png_write_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING,
      (png_voidp)nullptr, NULL, NULL);
  if (!png_write_ptr) {
    throw std::runtime_error("Error creating ping write info ptr");
  }

  // Have to use C-style FILE handle here, not easy to work around
  FILE *outFilePtr = fopen(outFilename, "wb");
  if (outFilePtr == nullptr) {
    throw std::runtime_error("Can't open file to write");
  }
  png_init_io(png_write_ptr, outFilePtr);
  
  // User state for writing
  struct PngReadWrite::userInfo info;
  info.png_write_ptr = png_write_ptr;
  info.sampleRate = sampleRate;
  png_set_progressive_read_fn(png_ptr, (void*)&info /* user pointer */,
        PngReadWrite::info_callback, PngReadWrite::row_callback, PngReadWrite::end_callback);
  png_set_interlace_handling(png_ptr);  
  //
  // end libpng boilerplate

  while (true) {
    // You can co_await a function that returns an Awaitable object,
    // or the awaitable object directly as we do here
    auto span = co_await imageReader;

    std::cout << "Read " << span.size() << " bytes" << std::endl;

    // at this point, the whole buffer chunk should be populated
    // process it through libpng
    // this will
    // - output png rows as they are decompressed
    // - write those lines incrementally back to a file
    // Note: for this to truly be 'low memory' would need to adjust zlib
    // window sizes to make sure a very small window is used
    //
    // Note: would be a cool project to make a fully coroutine-based png
    // processing library, but this would be a very nontrivial endeavour
    png_process_data(png_ptr, info_ptr, (png_bytep)span.data(), span.size()); 

    // Check if we are done reading, and therefore writing, the png
    if (info.isDone || span.size() == 0) {
      png_destroy_write_struct(&png_write_ptr,(png_infop*)nullptr);
      png_destroy_read_struct(&png_ptr,&info_ptr, (png_infop*)nullptr);
      fclose(outFilePtr);
      break;
    }

    // update when data translated
    std::cout << "Wrote " << ftell(outFilePtr) << " bytes" << std::endl;

    imageReader.clear();
  };

  // co_return is implied here
}


int main(int argc, char* argv[])
{
  if (argc != 4) {
    std::cout << "Required arguments: inFile outFile sampleRate" << std::endl;
    exit(-1);
  }

  int sampleRate = atoi(argv[3]);
  if (sampleRate <= 0) {
    std::cout << "Sample rate must be greater than 0" << std::endl;
    exit(-1);
  }

  auto handle = coPng(argv[1], argv[2], (unsigned)sampleRate).handle;
  auto &promise = handle.promise();
  std::cout << "Starting the png processing loop" << std::endl;
  while (!handle.done()) {
    handle(); // same as resume()
  }
  handle.destroy();
  return 0;
}
