# Slice Extractor

A library for rapid SVS tile extraction.

Machine learning in pathology oftentimes requires extracting patches from gigapixel files.

Existing solutions like slideio, openslide and CuCim provide toolsets for patch extraction from these slides.
These tools however are not fast enough for rapid tile extraction from different files for batches with a bigger size.
Hence I created this small library that focuses on this one functionality.

It offers:
- Memory efficiency
- Utilization of the fastest publicly available decoding suites
- Support for all JPEG encoding schemes used by Aperio scanners
- Faster extraction speeds than CuCim, slideio and openslide for the creation of a batch consisting of tiles from different slides
- Automatic selection and reading from the most fitting slide magnification level, if available

Tip: Use a DataLoader with multiple worker to overcome the latency of your file system. 

# Usage

You can simply download a precompiled `.so` file from here:
| File                                                                 | Architecture | OS     |
|----------------------------------------------------------------------|--------------|--------|
| [tiffsampler.so](https://hmgubox2.helmholtz-muenchen.de/index.php/s/edL572kZD3fEd9W) | x64          | Linux  |


Or you can compile it by yourself by downloading the [grok image codec suite](https://github.com/GrokImageCompression/grok) which thankfully already sets up all libraries this project uses with its cmake. The, use the `gcc` compiler with these flags:
```
gcc -shared -ltiff -ljpeg -lm -lgrokj2k -mavx512f -mavx512bw -mavx512vl tiff_sampler.c -o tiff_sampler.so -fPIC -O3
```

See `example.py` for an example of how to load a tile patch.

# Citation

If you find this repository useful, please consider citing it within your work:

```
@software{slice-extractor
    author="Christian Brechenmacher",
    title = {Slice Extractor},
    url = {https://github.com/ChrisNr1/slice-extractor},
    year = {2025}
}
```
