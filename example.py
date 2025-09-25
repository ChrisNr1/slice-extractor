from cffi import FFI
import torch
import torchvision.transforms.v2.functional as TF
import matplotlib.pyplot as plt

ffi = FFI()

ffi.cdef("""
typedef struct {
    uint8_t* tile;
    uint16_t tilelength;
    char first_dim;
} Tile;

Tile* load_single_tile(
    const char* fname,
    uint64_t x,
    uint64_t y,
    uint16_t tile_size,
    uint8_t magnification
);
        
void init_grok();
void free_tile(Tile* tile_ptr);
""")

# Load the shared library
lib = self.ffi.dlopen(os.path.join(os.environ['HOME'], 'HistoPath-RADIO/train_histo/tiff_sampler.so'))
lib.init_grok()

tile_ptr = self.lib.load_single_tile(
    "path/to/slide.svs".encode('utf-8'),
    100, # x coordinate
    200, # y coordinate
    256, # tile size
    10 # magnification
)

img = torch.frombuffer(self.ffi.buffer(tile_ptr.tile, tile_ptr.tilelength * tile_ptr.tilelength * 3), dtype=torch.uint8)

# for output in CxHxW
if tile_ptr.first_dim == b'H':
    img = img.reshape((tile_ptr.tilelength, tile_ptr.tilelength, 3))
    img = torch.moveaxis(img, -1, 0)
else: # first dim = C
    img = img.reshape((3, tile_ptr.tilelength, tile_ptr.tilelength))

img = TF.resize(img, (args.tile_size, args.tile_size)) # Copies the image and gets it into uniform size e.g. for eventual batching

lib.free_tile(tile_ptr) # free the C memory in which the original tile was stored

plt.imshow(img)
plt.show()
