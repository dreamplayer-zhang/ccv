#include <ccv.h>
#include <ccv_internal.h>
#include <nnc/ccv_nnc.h>
#include <nnc/ccv_nnc_easy.h>
#include <sys/time.h>
#include <ctype.h>
#include <getopt.h>
#include <stddef.h>
#include <3rdparty/khash/khash.h>

KHASH_MAP_INIT_STR(vocab_map, int)

static CCV_WARN_UNUSED(ccv_nnc_tensor_t*) _text_to_tensor_index(const char* const filename, const khash_t(vocab_map)* const vocab, const int vocab_size, const int max_length)
{
	const int end_flag = vocab_size - 2;
	const int pad_flag = vocab_size - 1;
	char* const word = (char*)ccmalloc(1024);
	ccv_nnc_tensor_t* const tensor = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32S, max_length), 0);
	FILE* const file = fopen(filename, "r");
	int t = 0;
	while (fscanf(file, "%1023s", word) != EOF)
	{
		if (t >= max_length)
			break;
		int j;
		for(j = 0; word[j]; j++)
			word[j] = tolower(word[j]);
		char* saveptr;
		const char* token = strtok_r(word, ".,<>/~`@#$%^&*+\\\"", &saveptr);
		while (token)
		{
			if (t >= max_length)
				break;
			const khiter_t k = kh_get(vocab_map, vocab, token);
			if (k != kh_end(vocab))
				tensor->data.i32[t++] = kh_val(vocab, k);
			token = strtok_r(0, ".,<>/~`@#$%^&*+\\\"", &saveptr);
		}
	}
	fclose(file);
	if (t < max_length)
	{
		tensor->data.i32[t] = end_flag;
		for (++t; t < max_length; t++)
			tensor->data.i32[t] = pad_flag;
	}
	ccfree(word);
	return tensor;
}

static ccv_array_t* _array_from_disk_new(const char* const list, const char* const base_dir, const khash_t(vocab_map)* const vocab, const int vocab_size, const int max_length)
{
	FILE *r = fopen(list, "r");
	assert(r && "list doesn't exists");
	int dirlen = (base_dir != 0) ? strlen(base_dir) + 1 : 0;
	ccv_array_t* categorizeds = ccv_array_new(sizeof(ccv_categorized_t), 64, 0);
	int c;
	char* file = (char*)ccmalloc(1024);
	char* filename = (char*)ccmalloc(1024);
	while (fscanf(r, "%d %1023s", &c, file) != EOF)
	{
		if (base_dir != 0)
		{
			strncpy(filename, base_dir, 1024);
			filename[dirlen - 1] = '/';
		}
		strncpy(filename + dirlen, file, 1024 - dirlen);
		ccv_nnc_tensor_t* const tensor = _text_to_tensor_index(filename, vocab, vocab_size, max_length);
		ccv_categorized_t categorized = ccv_categorized(c, (ccv_dense_matrix_t*)tensor, 0);
		ccv_array_push(categorizeds, &categorized);
	}
	ccfree(filename);
	ccfree(file);
	fclose(r);
	return categorizeds;
}

static ccv_cnnp_model_t* _self_attention_new(const int k, const int h, const int b, const int t)
{
	const ccv_cnnp_model_io_t x = ccv_cnnp_input();
	ccv_cnnp_model_t* const tokeys = ccv_cnnp_dense(k * h, (ccv_cnnp_param_t){
		.no_bias = 1,
	}, "tokeys");
	ccv_cnnp_model_t* const toqueries = ccv_cnnp_dense(k * h, (ccv_cnnp_param_t){
		.no_bias = 1,
	}, "toqueries");
	ccv_cnnp_model_t* const tovalues = ccv_cnnp_dense(k * h, (ccv_cnnp_param_t){
		.no_bias = 1,
	}, "tovalues");
	ccv_cnnp_model_io_t keys = ccv_cnnp_model_apply(tokeys, MODEL_IO_LIST(x));
	ccv_cnnp_model_io_t queries = ccv_cnnp_model_apply(toqueries, MODEL_IO_LIST(x));
	ccv_cnnp_model_io_t values = ccv_cnnp_model_apply(tovalues, MODEL_IO_LIST(x));
	keys = ccv_cnnp_model_apply(ccv_cnnp_reshape(DIM_ALLOC(b, t, h, k), DIM_ALLOC(), DIM_ALLOC(), 0), MODEL_IO_LIST(keys));
	queries = ccv_cnnp_model_apply(ccv_cnnp_reshape(DIM_ALLOC(b, t, h, k), DIM_ALLOC(), DIM_ALLOC(), 0), MODEL_IO_LIST(queries));
	values = ccv_cnnp_model_apply(ccv_cnnp_reshape(DIM_ALLOC(b, t, h, k), DIM_ALLOC(), DIM_ALLOC(), 0), MODEL_IO_LIST(values));
	keys = ccv_cnnp_model_apply(ccv_cnnp_cmd_exec(CMD_TRANSPOSE_FORWARD(1, 2), ccv_nnc_no_hint, 0, MODEL_CMD_EXEC_IO_MAP(KV(CCV_CNNP_IO)), MODEL_CMD_EXEC_IO_LIST(CCV_CNNP_IO), 0), MODEL_IO_LIST(keys));
	queries = ccv_cnnp_model_apply(ccv_cnnp_cmd_exec(CMD_TRANSPOSE_FORWARD(1, 2), ccv_nnc_no_hint, 0, MODEL_CMD_EXEC_IO_MAP(KV(CCV_CNNP_IO)), MODEL_CMD_EXEC_IO_LIST(CCV_CNNP_IO), 0), MODEL_IO_LIST(queries));
	values = ccv_cnnp_model_apply(ccv_cnnp_cmd_exec(CMD_TRANSPOSE_FORWARD(1, 2), ccv_nnc_no_hint, 0, MODEL_CMD_EXEC_IO_MAP(KV(CCV_CNNP_IO)), MODEL_CMD_EXEC_IO_LIST(CCV_CNNP_IO), 0), MODEL_IO_LIST(values));
	keys = ccv_cnnp_model_apply(ccv_cnnp_reshape(DIM_ALLOC(b * h, t, k), DIM_ALLOC(), DIM_ALLOC(), 0), MODEL_IO_LIST(keys));
	queries = ccv_cnnp_model_apply(ccv_cnnp_reshape(DIM_ALLOC(b * h, t, k), DIM_ALLOC(), DIM_ALLOC(), 0), MODEL_IO_LIST(queries));
	values = ccv_cnnp_model_apply(ccv_cnnp_reshape(DIM_ALLOC(b * h, t, k), DIM_ALLOC(), DIM_ALLOC(), 0), MODEL_IO_LIST(values));
	const float scale = 1. / powf(k, 0.25);
	queries = ccv_cnnp_model_apply(ccv_cnnp_cmd_exec(CMD_SCALAR_MUL_FORWARD(scale), ccv_nnc_no_hint, 0, MODEL_CMD_EXEC_IO_MAP(KV(CCV_CNNP_IO)), MODEL_CMD_EXEC_IO_LIST(CCV_CNNP_IO), 0), MODEL_IO_LIST(queries));
	keys = ccv_cnnp_model_apply(ccv_cnnp_cmd_exec(CMD_SCALAR_MUL_FORWARD(scale), ccv_nnc_no_hint, 0, MODEL_CMD_EXEC_IO_MAP(KV(CCV_CNNP_IO)), MODEL_CMD_EXEC_IO_LIST(CCV_CNNP_IO), 0), MODEL_IO_LIST(keys));
	ccv_cnnp_model_io_t keys_t = ccv_cnnp_model_apply(ccv_cnnp_cmd_exec(CMD_TRANSPOSE_FORWARD(1, 2), ccv_nnc_no_hint, 0, MODEL_CMD_EXEC_IO_MAP(KV(CCV_CNNP_IO)), MODEL_CMD_EXEC_IO_LIST(CCV_CNNP_IO), 0), MODEL_IO_LIST(keys));
	ccv_cnnp_model_io_t dot = ccv_cnnp_model_apply(ccv_cnnp_cmd_exec(CMD_GEMM_FORWARD(), ccv_nnc_no_hint, 0, MODEL_CMD_EXEC_IO_MAP(KV(CCV_CNNP_IO), KV(CCV_CNNP_IO)), MODEL_CMD_EXEC_IO_LIST(CCV_CNNP_IO), 0), MODEL_IO_LIST(queries, keys_t));
	dot = ccv_cnnp_model_apply(ccv_cnnp_identity((ccv_cnnp_param_t){
		.activation = CCV_CNNP_ACTIVATION_SOFTMAX,
	}, 0), MODEL_IO_LIST(dot));
	ccv_cnnp_model_io_t out = ccv_cnnp_model_apply(ccv_cnnp_cmd_exec(CMD_GEMM_FORWARD(), ccv_nnc_no_hint, 0, MODEL_CMD_EXEC_IO_MAP(KV(CCV_CNNP_IO), KV(CCV_CNNP_IO)), MODEL_CMD_EXEC_IO_LIST(CCV_CNNP_IO), 0), MODEL_IO_LIST(dot, values));
	out = ccv_cnnp_model_apply(ccv_cnnp_reshape(DIM_ALLOC(b, h, t, k), DIM_ALLOC(), DIM_ALLOC(), 0), MODEL_IO_LIST(out));
	out = ccv_cnnp_model_apply(ccv_cnnp_cmd_exec(CMD_TRANSPOSE_FORWARD(1, 2), ccv_nnc_no_hint, 0, MODEL_CMD_EXEC_IO_MAP(KV(CCV_CNNP_IO)), MODEL_CMD_EXEC_IO_LIST(CCV_CNNP_IO), 0), MODEL_IO_LIST(out));
	out = ccv_cnnp_model_apply(ccv_cnnp_reshape(DIM_ALLOC(b, t, h * k), DIM_ALLOC(), DIM_ALLOC(), 0), MODEL_IO_LIST(out));
	ccv_cnnp_model_t* const unifyheads = ccv_cnnp_dense(k, (ccv_cnnp_param_t){}, "unifyheads");
	out = ccv_cnnp_model_apply(unifyheads, MODEL_IO_LIST(out));
	return ccv_cnnp_model_new(MODEL_IO_LIST(x), MODEL_IO_LIST(out), "self-attention");
}

static ccv_cnnp_model_t* _transformer_block_new(const int k, const int h, const int b, const int t, const int ff)
{
	ccv_cnnp_model_io_t const x = ccv_cnnp_input();
	ccv_cnnp_model_t* const self_attention = _self_attention_new(k, h, b, t);
	ccv_cnnp_model_io_t out = ccv_cnnp_model_apply(self_attention, MODEL_IO_LIST(x));
	out = ccv_cnnp_model_apply(ccv_cnnp_add(0), MODEL_IO_LIST(x, out));
	out = ccv_cnnp_model_apply(ccv_cnnp_cmd_exec(CMD_LAYER_NORM_FORWARD(1e-4, 2), ccv_nnc_no_hint, 0, MODEL_CMD_EXEC_IO_MAP(KV(CCV_CNNP_IO), KV(CCV_CNNP_INIT_SHARED_TENSOR_AS_TRAINABLE, ccv_cnnp_cmd_exec_io_set_by(CMD_SET_FORWARD(1), ccv_nnc_no_hint, 0, GPU_TENSOR_NCHW(000, 32F, b, t, 1))), KV(CCV_CNNP_INIT_SHARED_TENSOR_AS_TRAINABLE, ccv_cnnp_cmd_exec_io_set_by(CMD_SET_FORWARD(0), ccv_nnc_no_hint, 0, GPU_TENSOR_NCHW(000, 32F, b, t, 1)))), MODEL_CMD_EXEC_IO_LIST(CCV_CNNP_IO, CCV_CNNP_TENSOR_NOT_OUTPUT, CCV_CNNP_TENSOR_NOT_OUTPUT), 0), MODEL_IO_LIST(out));
	out = ccv_cnnp_model_apply(ccv_cnnp_reshape(DIM_ALLOC(b * t, k), DIM_ALLOC(), DIM_ALLOC(), 0), MODEL_IO_LIST(out));
	out = ccv_cnnp_model_apply(ccv_cnnp_dense(ff, (ccv_cnnp_param_t){
		.activation = CCV_CNNP_ACTIVATION_RELU,
	}, 0), MODEL_IO_LIST(out));
	out = ccv_cnnp_model_apply(ccv_cnnp_dense(k, (ccv_cnnp_param_t){}, 0), MODEL_IO_LIST(out));
	out = ccv_cnnp_model_apply(ccv_cnnp_reshape(DIM_ALLOC(b, t, k), DIM_ALLOC(), DIM_ALLOC(), 0), MODEL_IO_LIST(out));
	out = ccv_cnnp_model_apply(ccv_cnnp_add(0), MODEL_IO_LIST(x, out));
	out = ccv_cnnp_model_apply(ccv_cnnp_cmd_exec(CMD_LAYER_NORM_FORWARD(1e-4, 2), ccv_nnc_no_hint, 0, MODEL_CMD_EXEC_IO_MAP(KV(CCV_CNNP_IO), KV(CCV_CNNP_INIT_SHARED_TENSOR_AS_TRAINABLE, ccv_cnnp_cmd_exec_io_set_by(CMD_SET_FORWARD(1), ccv_nnc_no_hint, 0, GPU_TENSOR_NCHW(000, 32F, b, t, 1))), KV(CCV_CNNP_INIT_SHARED_TENSOR_AS_TRAINABLE, ccv_cnnp_cmd_exec_io_set_by(CMD_SET_FORWARD(0), ccv_nnc_no_hint, 0, GPU_TENSOR_NCHW(000, 32F, b, t, 1)))), MODEL_CMD_EXEC_IO_LIST(CCV_CNNP_IO, CCV_CNNP_TENSOR_NOT_OUTPUT, CCV_CNNP_TENSOR_NOT_OUTPUT), 0), MODEL_IO_LIST(out));
	return ccv_cnnp_model_new(MODEL_IO_LIST(x), MODEL_IO_LIST(out), "transformer");
}

static void _vocab_init(const char* const vocab_file, khash_t(vocab_map)** const vocab_ref, int* const vocab_size_ref)
{
	FILE* const vocab_ptr = fopen(vocab_file, "r");
	khash_t(vocab_map)* const vocab = kh_init(vocab_map);
	int i, ret;
	char* const word = (char*)ccmalloc(1024);
	for (i = 0; fscanf(vocab_ptr, "%1023s", word) != EOF; i++)
	{
		const khiter_t k = kh_put(vocab_map, vocab, strdup(word), &ret);
		kh_val(vocab, k) = i;
	}
	ccfree(word);
	fclose(vocab_ptr);
	*vocab_ref = vocab;
	*vocab_size_ref = i;
}

static void _vocab_destroy(khash_t(vocab_map)* const vocab)
{
	// Free keys.
	for (khiter_t k = kh_begin(vocab); k != kh_end(vocab); k++)
		if (kh_exist(vocab, k))
			free((void*)kh_key(vocab, k));
	kh_destroy(vocab_map, vocab);
}

static void train_imdb(const int vocab_size, const int batch_size, const int max_length, const int embedding_size, ccv_cnnp_dataframe_t* const train_data, ccv_cnnp_dataframe_t* const test_data, ccv_array_t* const test_set)
{
	const int tensor_idx = ccv_cnnp_dataframe_extract_value(train_data, 0, offsetof(ccv_categorized_t, matrix));
	ccv_cnnp_dataframe_t* const batched_data = ccv_cnnp_dataframe_batching_new(train_data, COLUMN_ID_LIST(tensor_idx), batch_size, 1, CCV_TENSOR_FORMAT_NCHW);
	ccv_cnnp_dataframe_iter_t* const iter = ccv_cnnp_dataframe_iter_new(batched_data, COLUMN_ID_LIST(0));
	ccv_nnc_tensor_t** tensor;
	ccv_cnnp_dataframe_iter_next(iter, (void**)&tensor, 1, 0);
	ccv_nnc_tensor_t* const vocab_vec = ccv_nnc_tensor_new(0, CPU_TENSOR_NCHW(32F, vocab_size, embedding_size), 0);
	ccv_nnc_tensor_t* const seq_vec = ccv_nnc_tensor_new(0, CPU_TENSOR_NCHW(32F, max_length, embedding_size), 0);
	ccv_nnc_tensor_t* const seq_indices = ccv_nnc_tensor_new(0, CPU_TENSOR_NCHW(32S, batch_size * max_length), 0);
	ccv_nnc_cmd_exec(CMD_RANDOM_UNIFORM_FORWARD(-1, 1), ccv_nnc_no_hint, 0, TENSOR_LIST(), TENSOR_LIST(vocab_vec), 0);
	ccv_nnc_cmd_exec(CMD_RANDOM_UNIFORM_FORWARD(-1, 1), ccv_nnc_no_hint, 0, TENSOR_LIST(), TENSOR_LIST(seq_vec), 0);
	int i, j;
	for (i = 0; i < batch_size; i++)
		for (j = 0; j < max_length; j++)
			seq_indices->data.i32[i * max_length + j] = j;
	ccv_nnc_tensor_t* const vec = ccv_nnc_tensor_new(0, CPU_TENSOR_NCHW(32F, batch_size, max_length, embedding_size * 2), 0);
	ccv_nnc_tensor_t* const word_indices = ccv_nnc_tensor_new(tensor[0]->data.f32, CPU_TENSOR_NCHW(32S, batch_size * max_length), 0);
	ccv_nnc_tensor_view_t* const word_vec = ccv_nnc_tensor_view_new(vec, DIM_ALLOC(batch_size * max_length, embedding_size), DIM_ALLOC(), DIM_ALLOC(batch_size * max_length, embedding_size * 2));
	ccv_nnc_cmd_exec(CMD_INDEX_SELECT_FORWARD(), ccv_nnc_no_hint, 0, TENSOR_LIST(vocab_vec, word_indices), TENSOR_LIST((ccv_nnc_tensor_t*)word_vec), 0);
	ccv_nnc_tensor_view_t* const pos_vec = ccv_nnc_tensor_view_new(vec, DIM_ALLOC(batch_size * max_length, embedding_size), DIM_ALLOC(0, embedding_size), DIM_ALLOC(batch_size * max_length, embedding_size * 2));
	ccv_nnc_cmd_exec(CMD_INDEX_SELECT_FORWARD(), ccv_nnc_no_hint, 0, TENSOR_LIST(seq_vec, seq_indices), TENSOR_LIST((ccv_nnc_tensor_t*)pos_vec), 0);
	ccv_nnc_tensor_free(word_indices);
	ccv_nnc_tensor_view_free(word_vec);
	ccv_nnc_tensor_view_free(pos_vec);
	ccv_nnc_tensor_t* const gpu_vec = ccv_nnc_tensor_new(0, GPU_TENSOR_NCHW(000, 32F, batch_size, max_length, embedding_size * 2), 0);
	ccv_nnc_cmd_exec(CMD_DATA_TRANSFER_FORWARD(), ccv_nnc_no_hint, 0, TENSOR_LIST(vec), TENSOR_LIST(gpu_vec), 0);
	ccv_cnnp_model_t* const transformer = _transformer_block_new(embedding_size * 2, 8, batch_size, max_length, embedding_size * 8);
	ccv_cnnp_model_compile(transformer, TENSOR_PARAM_LIST(gpu_vec->info), CMD_NOOP(), CMD_NOOP());
	ccv_nnc_tensor_param_t out_params;
	ccv_cnnp_model_tensor_auto(transformer, &out_params, 1);
	ccv_nnc_tensor_t* const out = ccv_nnc_tensor_new(0, out_params, 0);
	CCV_CLI_SET_OUTPUT_LEVEL_AND_ABOVE(CCV_CLI_VERBOSE);
	ccv_cnnp_model_evaluate(transformer, (ccv_cnnp_evaluate_param_t){
		.requires_grad = 1,
	}, TENSOR_LIST(gpu_vec), TENSOR_LIST(out), 0, 0);
	ccv_cnnp_model_free(transformer);
	ccv_nnc_tensor_free(seq_indices);
	ccv_nnc_tensor_free(seq_vec);
	ccv_nnc_tensor_free(vocab_vec);
	ccv_nnc_tensor_free(vec);
	ccv_nnc_tensor_free(gpu_vec);
	ccv_nnc_tensor_free(out);
	ccv_cnnp_dataframe_iter_free(iter);
	ccv_cnnp_dataframe_free(batched_data);
}

int main(int argc, char** argv)
{
	ccv_nnc_init();
	static struct option imdb_options[] = {
		/* help */
		{"help", 0, 0, 0},
		/* required parameters */
		{"train-list", 1, 0, 0},
		{"test-list", 1, 0, 0},
		{"vocab", 1, 0, 0},
		/* optional parameters */
		{"base-dir", 1, 0, 0},
		{0, 0, 0, 0}
	};
	int c;
	char* train_list = 0;
	char* test_list = 0;
	char* base_dir = 0;
	char* vocab_file = 0;
	while (getopt_long_only(argc, argv, "", imdb_options, &c) != -1)
	{
		switch (c)
		{
			case 0:
				exit(0);
			case 1:
				train_list = optarg;
				break;
			case 2:
				test_list = optarg;
				break;
			case 3:
				vocab_file = optarg;
				break;
			case 4:
				base_dir = optarg;
				break;
		}
	}
	khash_t(vocab_map)* vocab;
	int vocab_size;
	_vocab_init(vocab_file, &vocab, &vocab_size);
	const int max_length = 512;
	ccv_array_t* const train_set = _array_from_disk_new(train_list, base_dir, vocab, vocab_size, max_length);
	ccv_cnnp_dataframe_t* const train_data = ccv_cnnp_dataframe_from_array_new(train_set);
	ccv_array_t* const test_set = _array_from_disk_new(test_list, base_dir, vocab, vocab_size, max_length);
	ccv_cnnp_dataframe_t* const test_data = ccv_cnnp_dataframe_from_array_new(test_set);
	train_imdb(vocab_size, 8, max_length, 128, train_data, test_data, test_set);
	ccv_cnnp_dataframe_free(train_data);
	ccv_cnnp_dataframe_free(test_data);
	int i;
	for (i = 0; i < train_set->rnum; i++)
		ccv_nnc_tensor_free((ccv_nnc_tensor_t*)((ccv_categorized_t*)ccv_array_get(train_set, i))->matrix);
	ccv_array_free(train_set);
	for (i = 0; i < test_set->rnum; i++)
		ccv_nnc_tensor_free((ccv_nnc_tensor_t*)((ccv_categorized_t*)ccv_array_get(test_set, i))->matrix);
	ccv_array_free(test_set);
	_vocab_destroy(vocab);
	return 0;
}