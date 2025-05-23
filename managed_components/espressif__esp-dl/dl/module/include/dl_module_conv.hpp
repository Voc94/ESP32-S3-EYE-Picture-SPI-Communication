#pragma once

#include "dl_base_conv2d.hpp"
#include "dl_base_depthwise_conv2d.hpp"
#include "dl_module_base.hpp"
#include <typeinfo>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace dl {
namespace module {

/**
 * @brief Activation(Conv2D(input, filter) + bias).
 *
 * @tparam feature_t supports int16_t and int8_t,
 *         - int16_t: stands for operation in int16_t quantize
 *         - int8_t: stands for operation in int8_t quantize
 */
class Conv2D : public Module {
private:
    const int stride_y;   /*!< stride in height */
    const int stride_x;   /*!< stride in width */
    const int dilation_y; /*!< dilation in height */
    const int dilation_x; /*!< dilation in width */
    const int group;
    activation_type_t activation; /*!< activation of Conv2D, if you don't specify anything, no activation is applied */
    std::vector<int> padding;     /*!< padding size needed in [top, bottom, left, right] of this operation */
    bool is_bias_reseted;

    void reset_bias(ModelContext *context)
    {
        if (is_bias_reseted == false) {
            if (m_inputs_index.size() == 3) {
                TensorBase *bias = context->get_tensor(m_inputs_index[2]);
                if (bias) {
                    bias->reset_bias_layout(quant_type, group != 1);
                }
            }
            is_bias_reseted = true;
        }
    }

public:
    /**
     * @brief Construct a new Conv2D object.
     *
     * @param activation      activation of Conv2D, if you don't specify anything, no activation is applied
     * @param padding         the shape must be 4, the value of each position is: [padding top, padding bottom, padding
     * left, padding right]
     * @param stride_y        stride in height
     * @param stride_x        stride in width
     * @param group           group of Conv
     * @param name            name of module
     */
    Conv2D(activation_type_t activation = Linear,
           std::vector<int> padding = {},
           const int stride_y = 1,
           const int stride_x = 1,
           const int dilation_y = 1,
           const int dilation_x = 1,
           const char *name = NULL,
           const int group = 1,
           quant_type_t quant_type = QUANT_TYPE_NONE) :
        Module(name, MODULE_NON_INPLACE, quant_type),
        stride_y(stride_y),
        stride_x(stride_x),
        dilation_y(dilation_y),
        dilation_x(dilation_x),
        group(group),
        activation(activation),
        padding(padding)
    {
        is_bias_reseted = false;
    }

    /**
     * @brief Destroy the Conv2D object.
     *
     */
    ~Conv2D() {}

    /**
     * @brief Calculate the output shape
     *
     * @param input_shape The shape of inputs
     *
     * @return output shape
     */
    std::vector<std::vector<int>> get_output_shape(std::vector<std::vector<int>> &input_shapes)
    {
        assert(input_shapes.size() >= 2);
        assert(input_shapes[0].size() == 4);
        int *input_shape = input_shapes[0].data();
        int *filter_shape = input_shapes[1].data();
        std::vector<int> output_shape(4);

        // refer to https://pytorch.org/docs/stable/generated/torch.nn.Conv2d.html
        output_shape[0] = input_shape[0];
        output_shape[1] =
            (input_shape[1] + padding[0] + padding[1] - dilation_y * (filter_shape[0] - 1) - 1) / stride_y + 1;
        output_shape[2] =
            (input_shape[2] + padding[2] + padding[3] - dilation_x * (filter_shape[1] - 1) - 1) / stride_x + 1;
        output_shape[3] = group == 1 ? filter_shape[3] : input_shape[3];

        std::vector<std::vector<int>> output_shapes(1, output_shape);
        return output_shapes;
    }

    void forward_args(void *args)
    {
        if (group == 1) {
            if (quant_type == QUANT_TYPE_SYMM_8BIT) {
                base::conv2d<int8_t, int32_t, int32_t>(args);
            } else if (quant_type == QUANT_TYPE_SYMM_16BIT) {
                base::conv2d<int16_t, int32_t, int64_t>(args);
            }
        } else {
            if (quant_type == QUANT_TYPE_SYMM_8BIT) {
                base::depthwise_conv2d<int8_t, int32_t, int32_t>(args);
            } else if (quant_type == QUANT_TYPE_SYMM_16BIT) {
                base::depthwise_conv2d<int16_t, int32_t, int64_t>(args);
            }
        }
    }

    void forward(ModelContext *context, runtime_mode_t mode = RUNTIME_MODE_AUTO)
    {
        reset_bias(context);

        if (quant_type == QUANT_TYPE_SYMM_8BIT) {
            forward_template<int8_t>(context, mode);
        } else if (quant_type == QUANT_TYPE_SYMM_16BIT) {
            forward_template<int16_t>(context, mode);
        }
    }

    template <typename T>
    void forward_template(ModelContext *context, runtime_mode_t mode)
    {
        TensorBase *input = context->get_tensor(m_inputs_index[0]);
        TensorBase *filter = context->get_tensor(m_inputs_index[1]);
        TensorBase *bias = nullptr;
        if (m_inputs_index.size() == 3) {
            bias = context->get_tensor(m_inputs_index[2]);
        }
        TensorBase *output = context->get_tensor(m_outputs_index[0]);

        std::vector<base::ArgsType<T>> m_args =
            base::get_conv_operation_args<T>(output,
                                             input,
                                             this->padding,
                                             filter,
                                             this->stride_y,
                                             this->stride_x,
                                             this->dilation_y,
                                             this->dilation_x,
                                             this->group,
                                             bias,
                                             this->activation,
                                             nullptr,
                                             mode); // do not support RReLU and Leaky RelU
        int task_size = m_args.size();
        if (task_size == 1) { // single task
            forward_args((void *)&m_args[0]);
        } else if (task_size == 2) { // multi task, use semaphore to maintain synchronization.
            module_forward_dual_core(this, (void *)&m_args[0], (void *)&m_args[1]);
        } else {
            ESP_LOGE("Conv2D", "Only support task size is 1 or 2, currently task size is %d", task_size);
        }
    }

    /**
     * @brief deserialize Conv2d module instance by node serialization information
     */
    static Module *deserialize(fbs::FbsModel *fbs_model, std::string node_name)
    {
        Module *conv2d_op = nullptr;

        std::vector<int> pads;
        std::vector<int> strides;
        std::vector<int> dilations;
        int group = 1;
        activation_type_t activation_type;
        quant_type_t quant_type;
        fbs_model->get_operation_attribute(node_name, "pads", pads);
        fbs_model->get_operation_attribute(node_name, "strides", strides);
        fbs_model->get_operation_attribute(node_name, "dilations", dilations);
        fbs_model->get_operation_attribute(node_name, "group", group);
        fbs_model->get_operation_attribute(node_name, "activation", activation_type);
        fbs_model->get_operation_attribute(node_name, "quant_type", quant_type);

        // Create module
        if (quant_type == QUANT_TYPE_SYMM_8BIT || quant_type == QUANT_TYPE_SYMM_16BIT) {
            conv2d_op = new Conv2D(activation_type,
                                   {pads[0], pads[2], pads[1], pads[3]},
                                   strides[0],
                                   strides[1],
                                   dilations[0],
                                   dilations[1],
                                   node_name.c_str(),
                                   group,
                                   quant_type);
        }

        return conv2d_op;
    }

    void print()
    {
        ESP_LOGI("Conv2d",
                 "pads: %s, strides: [%d,%d], dilations: [%d,%d], group: %d, activation: %s, "
                 "quant_type: %s.",
                 shape_to_string(padding).c_str(),
                 stride_y,
                 stride_x,
                 dilation_y,
                 dilation_x,
                 group,
                 activation_type_to_string(activation),
                 quant_type_to_string(quant_type));
    }

    // void set_preload_addr(void *addr, size_t size)
    // {
    //     size_t offset = 0;
    //     if (this->filter) {
    //         offset = this->filter->set_preload_addr(addr, size);
    //     }
    //     if (this->bias) {
    //         this->bias->set_preload_addr((void *)((char *)addr + offset), size - offset);
    //     }
    // }

    // void preload()
    // {
    //     // printf("preload filter and bias!");
    //     if (filter)
    //         filter->preload();
    //     if (bias)
    //         bias->preload();
    // }

    // void reset()
    // {
    //     this->m_inputs_index.clear();
    //     this->m_outputs_index.clear();
    //     this->filter->cache = nullptr;
    //     if (this->bias != nullptr) {
    //         this->bias->cache = nullptr;
    //     }
    // }
};
} // namespace module
} // namespace dl
