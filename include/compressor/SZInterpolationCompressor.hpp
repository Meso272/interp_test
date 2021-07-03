#ifndef _SZ_SZ_FAST_INTERPOLATION_HPP
#define _SZ_SZ_FAST_INTERPOLATION_HPP

#include "predictor/Predictor.hpp"
#include "predictor/LorenzoPredictor.hpp"
#include "quantizer/Quantizer.hpp"
#include "encoder/Encoder.hpp"
#include "lossless/Lossless.hpp"
#include "utils/Iterator.hpp"
#include "utils/MemoryUtil.hpp"
#include "utils/Config.hpp"
#include "utils/FileUtil.h"
#include "utils/Interpolators.hpp"
#include "utils/Timer.hpp"
#include "def.hpp"
#include <cstring>
#include <cmath>

namespace SZ {
    template<class T, uint N, class Quantizer, class Encoder, class Lossless>
    class SZInterpolationCompressor {
    public:


        SZInterpolationCompressor(Quantizer quantizer, Encoder encoder, Lossless lossless,
                                  const std::array<size_t, N> dims,
                                  size_t blocksize,
                                  int interpolator,
                                  int direction,
                                  int interp_levels) :
                quantizer(quantizer), encoder(encoder), lossless(lossless),
                blocksize(blocksize), global_dimensions(dims),
//                interpolators({"linear", "cubic", "cubic2", "akima", "pchip"}),
                interpolators({"linear", "cubic"}),
                interpolator_id(interpolator), direction_sequence_id(direction) {
            static_assert(std::is_base_of<concepts::QuantizerInterface<T>, Quantizer>::value,
                          "must implement the quatizer interface");
            static_assert(std::is_base_of<concepts::EncoderInterface<int>, Encoder>::value,
                          "must implement the encoder interface");
            static_assert(std::is_base_of<concepts::LosslessInterface, Lossless>::value,
                          "must implement the lossless interface");

            assert(blocksize % 2 == 0 && "Interpolation block size should be even numbers");
            num_elements = 1;
            interpolation_level = -1;
            for (int i = 0; i < N; i++) {
                if (interpolation_level < ceil(log2(dims[i]))) {
                    interpolation_level = (uint) ceil(log2(dims[i]));
                }
                num_elements *= dims[i];
            }

            dimension_offsets[N - 1] = 1;
            for (int i = N - 2; i >= 0; i--) {
                dimension_offsets[i] = dimension_offsets[i + 1] * global_dimensions[i + 1];
            }

            dimension_sequences = std::vector<std::array<int, N>>();
            auto sequence = std::array<int, N>();
            for (int i = 0; i < N; i++) {
                sequence[i] = i;
            }
            do {
                dimension_sequences.push_back(sequence);
            } while (std::next_permutation(sequence.begin(), sequence.end()));


            size_t num_params=0;
            

            auto c_params=SZ::readfile<T>("c_params.dat",num_params);
            if (num_params!=4){
                std::cout<<"Read params failed."<<endl;
                c_a=c_d=-1/16;
                c_b=c_c=9/16;
            }
            else{
                c_a=c_params[0];
                c_b=c_params[1];
                c_c=c_params[2];
                c_d=c_params[3];
            }



        }


        T *decompress(uchar *compressed_data, const size_t length, bool pre_de_lossless = false) {
            size_t remaining_length = length;
            uchar *lossless_decompressed;
            uchar const *compressed_data_pos;
            if (pre_de_lossless) {
                compressed_data_pos = compressed_data;
            } else {
                lossless_decompressed = lossless.decompress(compressed_data, remaining_length);
                compressed_data_pos = lossless_decompressed;
                T eb;
                read(eb, compressed_data_pos, remaining_length);
            }

            read(global_dimensions.data(), N, compressed_data_pos, remaining_length);
            num_elements = 1;
            for (const auto &d : global_dimensions) {
                num_elements *= d;
                std::cout << d << " ";
            }
            std::cout << std::endl;
            uint block_size = 0;
            read(block_size, compressed_data_pos, remaining_length);
            quantizer.load(compressed_data_pos, remaining_length);
            encoder.load(compressed_data_pos, remaining_length);
            quant_inds = encoder.decode(compressed_data_pos, num_elements);

            encoder.postprocess_decode();

            if (!pre_de_lossless) {
                lossless.postdecompress_data(lossless_decompressed);
            }
            double eb = quantizer.get_eb();
//            quantizer.set_eb(eb * eb_ratio);

//            auto dec_data = std::make_shared<T[]>(num_elements);
            T *dec_data = new T[num_elements];

//            quantizer.set_eb(eb);
            Timer timer;
            timer.start();

            *dec_data = quantizer.recover(0, quant_inds[quant_index++]);

            for (uint level = interpolation_level; level > 0 && level <= interpolation_level; level--) {
                if (level >= 3) {
                    quantizer.set_eb(eb * eb_ratio);
                } else {
                    quantizer.set_eb(eb);
                }
                size_t stride = 1U << (level - 1);
                auto inter_block_range = std::make_shared<
                        SZ::multi_dimensional_range<T, N>>(dec_data,
                                                           std::begin(global_dimensions), std::end(global_dimensions),
                                                           stride * blocksize, 0);
                auto inter_begin = inter_block_range->begin();
                auto inter_end = inter_block_range->end();
                for (auto block = inter_begin; block != inter_end; ++block) {
                    auto end_idx = block.get_global_index();
                    for (int i = 0; i < N; i++) {
                        end_idx[i] += stride * blocksize;
                        if (end_idx[i] > global_dimensions[i] - 1) {
                            end_idx[i] = global_dimensions[i] - 1;
                        }
                    }
                    block_interpolation(dec_data, block.get_global_index(), end_idx, PB_recover,
                                        interpolators[interpolator_id], direction_sequence_id, stride);
                }
            }
            std::cout << "Total quant element = " << quant_inds.size() << std::endl;
            quantizer.postdecompress_data();
            timer.stop("Interpolation Decompress");

            return dec_data;
        }

        uchar *compress(T *data, size_t &compressed_size) {
            return compress(data, compressed_size, false);
        }

        // compress given the error bound
        uchar *compress(T *data, size_t &compressed_size, bool deleteData) {
            quant_inds.reserve(num_elements);
//            quant_inds.resize(num_elements);
            size_t interp_compressed_size = 0;
//            debug.resize(num_elements, 0);
//            preds.resize(num_elements, 0);

            T eb = quantizer.get_eb();
            std::cout << "Absolute error bound = " << eb << std::endl;
//            quantizer.set_eb(eb * eb_ratio);

//            quant_inds[0] = quantizer.quantize_and_overwrite(*data, 0);
//            preds[0] = 0;
            quant_inds.push_back(quantizer.quantize_and_overwrite(*data, 0));

            Timer timer;
            timer.start();

            for (uint level = interpolation_level; level > 0 && level <= interpolation_level; level--) {
                if (level >= 3) {
                    quantizer.set_eb(eb * eb_ratio);
                } else {
                    quantizer.set_eb(eb);
                }
                uint stride = 1U << (level - 1);
//                std::cout << "Level = " << level << ", stride = " << stride << std::endl;

                auto inter_block_range = std::make_shared<
                        SZ::multi_dimensional_range<T, N>>(data, std::begin(global_dimensions),
                                                           std::end(global_dimensions),
                                                           blocksize * stride, 0);

                auto inter_begin = inter_block_range->begin();
                auto inter_end = inter_block_range->end();
                for (auto block = inter_begin; block != inter_end; ++block) {
                    auto end_idx = block.get_global_index();
                    for (int i = 0; i < N; i++) {
                        end_idx[i] += blocksize * stride;
                        if (end_idx[i] > global_dimensions[i] - 1) {
                            end_idx[i] = global_dimensions[i] - 1;
                        }
                    }

                    block_interpolation(data, block.get_global_index(), end_idx, PB_predict_overwrite,
                                        interpolators[interpolator_id], direction_sequence_id, stride);
                }
            }
            if (deleteData) {
                delete[]data;
            }
            std::cout << "total element = " << num_elements << std::endl;
            std::cout << "quantization element = " << quant_inds.size() << std::endl;
            assert(quant_inds.size() == num_elements);
            timer.stop("Predition & Quantization");

//            writefile("pred.dat", preds.data(), num_elements);
//            writefile("quant.dat", quant_inds.data(), num_elements);
            //TODO find a better estimation method
            uchar *compressed_data = (num_elements < 1000000) ?
                                     new uchar[4 * num_elements * sizeof(T)] :
                                     new uchar[size_t(1.2 * num_elements) * sizeof(T)];
            uchar *compressed_data_pos = compressed_data;
            write(eb, compressed_data_pos);
            write(global_dimensions.data(), N, compressed_data_pos);
            write(blocksize, compressed_data_pos);

            quantizer.save(compressed_data_pos);
            quantizer.postcompress_data();

            encoder.preprocess_encode(quant_inds, 4 * quantizer.get_radius());
            encoder.save(compressed_data_pos);
            encoder.encode(quant_inds, compressed_data_pos);
            encoder.postprocess_encode();

            uchar *lossless_data = lossless.compress(compressed_data,
                                                     compressed_data_pos - compressed_data,
                                                     compressed_size);
            lossless.postcompress_data(compressed_data);

            compressed_size += interp_compressed_size;
            return lossless_data;
        }

    private:

        enum PredictorBehavior {
            PB_predict_overwrite, PB_predict, PB_recover
        };

        inline void quantize1(size_t idx, T &d, T pred) {
            if (idx >= 2 * 449 * 449 * 235 && idx < 3 * 449 * 449 * 235 &&
                fabs(d - pred) > max_error) {
                max_error = fabs(d - pred);
            }
            auto quant = quantizer.quantize_and_overwrite(d, pred);
//            quant_inds.push_back(quant);
            quant_inds[idx] = quant;
        }

        inline void quantize(size_t idx, T &d, T pred) {
//            preds[idx] = pred;
//            quant_inds[idx] = quantizer.quantize_and_overwrite(d, pred);
            quant_inds.push_back(quantizer.quantize_and_overwrite(d, pred));
        }

        inline void recover(T &d, T pred) {
            d = quantizer.recover(pred, quant_inds[quant_index++]);
        };


        double block_interpolation_1d(T *data, size_t begin, size_t end, size_t stride,
                                      const std::string &interp_func,
                                      const PredictorBehavior pb) {
            size_t n = (end - begin) / stride + 1;
            if (n <= 1) {
                return 0;
            }
            double predict_error = 0;

            size_t stride3x = 3 * stride;
            size_t stride5x = 5 * stride;
            if (interp_func == "linear" || n < 5) {
                if (pb == PB_predict_overwrite) {
                    for (size_t i = 1; i + 1 < n; i += 2) {
                        T *d = data + begin + i * stride;
                        quantize(d - data, *d, interp_linear(*(d - stride), *(d + stride)));
                    }
                    if (n % 2 == 0) {
                        T *d = data + begin + (n - 1) * stride;
                        if (n < 4) {
                            quantize(d - data, *d, *(d - stride));
                        } else {
                            quantize(d - data, *d, interp_linear1(*(d - stride3x), *(d - stride)));
                        }
                    }
                } else {
                    for (size_t i = 1; i + 1 < n; i += 2) {
                        T *d = data + begin + i * stride;
                        recover(*d, interp_linear(*(d - stride), *(d + stride)));
                    }
                    if (n % 2 == 0) {
                        T *d = data + begin + (n - 1) * stride;
                        if (n < 4) {
                            recover(*d, *(d - stride));
                        } else {
                            recover(*d, interp_linear1(*(d - stride3x), *(d - stride)));
                        }
                    }
                }
            } else {
                if (pb == PB_predict_overwrite) {

                    T *d;
                    size_t i;
                    for (i = 3; i + 3 < n; i += 2) {
                        d = data + begin + i * stride;
                        if (stride!=1){
                        quantize(d - data, *d,
                                 interp_cubic(*(d - stride3x), *(d - stride), *(d + stride), *(d + stride3x)));
                        }
                        else{
                            quantize(d - data, *d,
                                 interp_cubic_p(*(d - stride3x), *(d - stride), *(d + stride), *(d + stride3x),c_a,c_b,c_c,c_d ));
                        }
                    }
                    d = data + begin + stride;
                    quantize(d - data, *d, interp_quad_1(*(d - stride), *(d + stride), *(d + stride3x)));

                    d = data + begin + i * stride;
                    quantize(d - data, *d, interp_quad_2(*(d - stride3x), *(d - stride), *(d + stride)));
                    if (n % 2 == 0) {
                        d = data + begin + (n - 1) * stride;
                        quantize(d - data, *d, interp_quad_3(*(d - stride5x), *(d - stride3x), *(d - stride)));
                    }

                } else {
                    T *d;

                    size_t i;
                    for (i = 3; i + 3 < n; i += 2) {
                        d = data + begin + i * stride;
                        if(stride!=1)
                        recover(*d, interp_cubic(*(d - stride3x), *(d - stride), *(d + stride), *(d + stride3x)));
                        else
                        recover(*d, interp_cubic_p(*(d - stride3x), *(d - stride), *(d + stride), *(d + stride3x),c_a,c_b,c_c,c_d ));
                    }
                    d = data + begin + stride;

                    recover(*d, interp_quad_1(*(d - stride), *(d + stride), *(d + stride3x)));

                    d = data + begin + i * stride;
                    recover(*d, interp_quad_2(*(d - stride3x), *(d - stride), *(d + stride)));

                    if (n % 2 == 0) {
                        d = data + begin + (n - 1) * stride;
                        recover(*d, interp_quad_3(*(d - stride5x), *(d - stride3x), *(d - stride)));
                    }
                }
            }

            return predict_error;
        }

        template<uint NN = N>
        typename std::enable_if<NN == 1, double>::type
        block_interpolation(T *data, std::array<size_t, N> begin, std::array<size_t, N> end, const PredictorBehavior pb,
                            const std::string &interp_func, const int direction, uint stride = 1) {
            return block_interpolation_1d(data, begin[0], end[0], stride, interp_func, pb);
        }

        template<uint NN = N>
        typename std::enable_if<NN == 2, double>::type
        block_interpolation(T *data, std::array<size_t, N> begin, std::array<size_t, N> end, const PredictorBehavior pb,
                            const std::string &interp_func, const int direction, uint stride = 1) {
            double predict_error = 0;
            size_t stride2x = stride * 2;
            std::array<int, N> dims = dimension_sequences[direction];
            for (size_t j = (begin[dims[1]] ? begin[dims[1]] + stride2x : 0); j <= end[dims[1]]; j += stride2x) {
                size_t begin_offset = begin[dims[0]] * dimension_offsets[dims[0]] + j * dimension_offsets[dims[1]];
                predict_error += block_interpolation_1d(data, begin_offset,
                                                        begin_offset +
                                                        (end[dims[0]] - begin[dims[0]]) * dimension_offsets[dims[0]],
                                                        stride * dimension_offsets[dims[0]], interp_func, pb);
            }
            for (size_t i = (begin[dims[0]] ? begin[dims[0]] + stride : 0); i <= end[dims[0]]; i += stride) {
                size_t begin_offset = i * dimension_offsets[dims[0]] + begin[dims[1]] * dimension_offsets[dims[1]];
                predict_error += block_interpolation_1d(data, begin_offset,
                                                        begin_offset +
                                                        (end[dims[1]] - begin[dims[1]]) * dimension_offsets[dims[1]],
                                                        stride * dimension_offsets[dims[1]], interp_func, pb);
            }
            return predict_error;
        }

        template<uint NN = N>
        typename std::enable_if<NN == 3, double>::type
        block_interpolation(T *data, std::array<size_t, N> begin, std::array<size_t, N> end, const PredictorBehavior pb,
                            const std::string &interp_func, const int direction, uint stride = 1) {
            double predict_error = 0;
            size_t stride2x = stride * 2;
            std::array<int, N> dims = dimension_sequences[direction];
            for (size_t j = (begin[dims[1]] ? begin[dims[1]] + stride2x : 0); j <= end[dims[1]]; j += stride2x) {
                for (size_t k = (begin[dims[2]] ? begin[dims[2]] + stride2x : 0); k <= end[dims[2]]; k += stride2x) {
                    size_t begin_offset = begin[dims[0]] * dimension_offsets[dims[0]] + j * dimension_offsets[dims[1]] +
                                          k * dimension_offsets[dims[2]];
                    predict_error += block_interpolation_1d(data, begin_offset,
                                                            begin_offset +
                                                            (end[dims[0]] - begin[dims[0]]) *
                                                            dimension_offsets[dims[0]],
                                                            stride * dimension_offsets[dims[0]], interp_func, pb);
                }
            }
            for (size_t i = (begin[dims[0]] ? begin[dims[0]] + stride : 0); i <= end[dims[0]]; i += stride) {
                for (size_t k = (begin[dims[2]] ? begin[dims[2]] + stride2x : 0); k <= end[dims[2]]; k += stride2x) {
                    size_t begin_offset = i * dimension_offsets[dims[0]] + begin[dims[1]] * dimension_offsets[dims[1]] +
                                          k * dimension_offsets[dims[2]];
                    predict_error += block_interpolation_1d(data, begin_offset,
                                                            begin_offset +
                                                            (end[dims[1]] - begin[dims[1]]) *
                                                            dimension_offsets[dims[1]],
                                                            stride * dimension_offsets[dims[1]], interp_func, pb);
                }
            }
            for (size_t i = (begin[dims[0]] ? begin[dims[0]] + stride : 0); i <= end[dims[0]]; i += stride) {
                for (size_t j = (begin[dims[1]] ? begin[dims[1]] + stride : 0); j <= end[dims[1]]; j += stride) {
                    size_t begin_offset = i * dimension_offsets[dims[0]] + j * dimension_offsets[dims[1]] +
                                          begin[dims[2]] * dimension_offsets[dims[2]];
                    predict_error += block_interpolation_1d(data, begin_offset,
                                                            begin_offset +
                                                            (end[dims[2]] - begin[dims[2]]) *
                                                            dimension_offsets[dims[2]],
                                                            stride * dimension_offsets[dims[2]], interp_func, pb);
                }
            }
            return predict_error;
        }


        template<uint NN = N>
        typename std::enable_if<NN == 4, double>::type
        block_interpolation(T *data, std::array<size_t, N> begin, std::array<size_t, N> end, const PredictorBehavior pb,
                            const std::string &interp_func, const int direction, uint stride = 1) {
            double predict_error = 0;
            size_t stride2x = stride * 2;
            max_error = 0;
            std::array<int, N> dims = dimension_sequences[direction];
            for (size_t j = (begin[dims[1]] ? begin[dims[1]] + stride2x : 0); j <= end[dims[1]]; j += stride2x) {
                for (size_t k = (begin[dims[2]] ? begin[dims[2]] + stride2x : 0); k <= end[dims[2]]; k += stride2x) {
                    for (size_t t = (begin[dims[3]] ? begin[dims[3]] + stride2x : 0);
                         t <= end[dims[3]]; t += stride2x) {
                        size_t begin_offset =
                                begin[dims[0]] * dimension_offsets[dims[0]] + j * dimension_offsets[dims[1]] +
                                k * dimension_offsets[dims[2]] +
                                t * dimension_offsets[dims[3]];
                        predict_error += block_interpolation_1d(data, begin_offset,
                                                                begin_offset +
                                                                (end[dims[0]] - begin[dims[0]]) *
                                                                dimension_offsets[dims[0]],
                                                                stride * dimension_offsets[dims[0]], interp_func, pb);
                    }
                }
            }
//            printf("%.8f ", max_error);
            max_error = 0;
            for (size_t i = (begin[dims[0]] ? begin[dims[0]] + stride : 0); i <= end[dims[0]]; i += stride) {
                for (size_t k = (begin[dims[2]] ? begin[dims[2]] + stride2x : 0); k <= end[dims[2]]; k += stride2x) {
                    for (size_t t = (begin[dims[3]] ? begin[dims[3]] + stride2x : 0);
                         t <= end[dims[3]]; t += stride2x) {
                        size_t begin_offset =
                                i * dimension_offsets[dims[0]] + begin[dims[1]] * dimension_offsets[dims[1]] +
                                k * dimension_offsets[dims[2]] +
                                t * dimension_offsets[dims[3]];
                        predict_error += block_interpolation_1d(data, begin_offset,
                                                                begin_offset +
                                                                (end[dims[1]] - begin[dims[1]]) *
                                                                dimension_offsets[dims[1]],
                                                                stride * dimension_offsets[dims[1]], interp_func, pb);
                    }
                }
            }
//            printf("%.8f ", max_error);
            max_error = 0;
            for (size_t i = (begin[dims[0]] ? begin[dims[0]] + stride : 0); i <= end[dims[0]]; i += stride) {
                for (size_t j = (begin[dims[1]] ? begin[dims[1]] + stride : 0); j <= end[dims[1]]; j += stride) {
                    for (size_t t = (begin[dims[3]] ? begin[dims[3]] + stride2x : 0);
                         t <= end[dims[3]]; t += stride2x) {
                        size_t begin_offset = i * dimension_offsets[dims[0]] + j * dimension_offsets[dims[1]] +
                                              begin[dims[2]] * dimension_offsets[dims[2]] +
                                              t * dimension_offsets[dims[3]];
                        predict_error += block_interpolation_1d(data, begin_offset,
                                                                begin_offset +
                                                                (end[dims[2]] - begin[dims[2]]) *
                                                                dimension_offsets[dims[2]],
                                                                stride * dimension_offsets[dims[2]], interp_func, pb);
                    }
                }
            }

//            printf("%.8f ", max_error);
            max_error = 0;
            for (size_t i = (begin[dims[0]] ? begin[dims[0]] + stride : 0); i <= end[dims[0]]; i += stride) {
                for (size_t j = (begin[dims[1]] ? begin[dims[1]] + stride : 0); j <= end[dims[1]]; j += stride) {
                    for (size_t k = (begin[dims[2]] ? begin[dims[2]] + stride : 0); k <= end[dims[2]]; k += stride) {
                        size_t begin_offset =
                                i * dimension_offsets[dims[0]] + j * dimension_offsets[dims[1]] +
                                k * dimension_offsets[dims[2]] +
                                begin[dims[3]] * dimension_offsets[dims[3]];
                        predict_error += block_interpolation_1d(data, begin_offset,
                                                                begin_offset +
                                                                (end[dims[3]] - begin[dims[3]]) *
                                                                dimension_offsets[dims[3]],
                                                                stride * dimension_offsets[dims[3]], interp_func, pb);
                    }
                }
            }
//            printf("%.8f \n", max_error);
            return predict_error;
        }

        int interpolation_level = -1;
        uint blocksize;
        int interpolator_id;
        double eb_ratio = 0.5;
        std::vector<std::string> interpolators;
        std::vector<int> quant_inds;
        size_t quant_index = 0; // for decompress
//        std::vector<int> debug;
//        std::vector<T> preds;
        double max_error;
        Quantizer quantizer;
        Encoder encoder;
        Lossless lossless;
        size_t num_elements;
        std::array<size_t, N> global_dimensions;
        std::array<size_t, N> dimension_offsets;
        std::vector<std::array<int, N>> dimension_sequences;
        int direction_sequence_id;
        T c_a,c_b,c_c,c_d;
    };


};


#endif

