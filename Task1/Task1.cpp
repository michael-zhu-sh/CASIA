/*
ICDAR 2013大赛任务1，提取特征数据的分类。
*/
#include "stdafx.h"
#include <io.h>

#include <opencv2/opencv.hpp>
#include <opencv2/core/ocl.hpp>

using namespace std;
using namespace cv;
using namespace cv::ml;

struct MPF_HEADER1 {
	unsigned int headerSize;
	unsigned char formatCode[8];
	unsigned char buf[512];
};
struct MPF_HEADER2 {
	unsigned char codeType[20];
	unsigned short codeLength;
	unsigned char dataType[20];
	unsigned char cSampleNumber[4];
	unsigned char cDimension[4];
};

/*MPF*/
string MPF_TRAINSET_DIR;//MPF训练集目录。
string MPF_TESTSET_DIR;	//MPF测试集目录。
const size_t MPF_FEATURE_DIMENSION = 512;
const size_t MPF_FEATURE_SIZE = MPF_FEATURE_DIMENSION + 2;		//MPF文件中的特征数据长度。
Mat TRAIN_MPF_FEATURE_MAT(0, MPF_FEATURE_DIMENSION, CV_32FC1);	//存放MPF训练集所有的特征向量。
Mat TRAIN_MPF_LABEL_MAT(0, 1, CV_32SC1);
cv::UMat TRAIN_FEATURE_UMAT;
cv::UMat TRAIN_LABEL_UMAT;
Mat PCA_FEATURE_MAT;
/*MPF*/

cv::Ptr<SVM> LINEAR_SVM_PTR;
cv::Ptr<SVM> RBF_SVM_PTR;
cv::Ptr<ANN_MLP> ANN_MLP_PTR;

/*===========================================================================*/
int getGBFrom2Char(unsigned char code1, unsigned char code2) {
	unsigned short tag, ltag, rtag;
	tag = code1;
	ltag = tag << 8;
	rtag = code2;
	return (ltag | rtag);
}

/*
找到指定目录下所有的文件。
*/
void getAllFiles(const string &path, vector<string> &files)
{
	//文件句柄  
	intptr_t hFile = 0;
	//文件信息  
	struct _finddata_t fileinfo;  //很少用的文件信息读取结构
	string p;  //string类很有意思的一个赋值函数:assign()，有很多重载版本
	if ((hFile = _findfirst(p.assign(path).append("\\*").c_str(), &fileinfo)) != -1)
	{
		do
		{
			if ((fileinfo.attrib &  _A_SUBDIR))  //比较文件类型是否是文件夹
			{
				if (strcmp(fileinfo.name, ".") != 0 && strcmp(fileinfo.name, "..") != 0)
				{
					files.push_back(p.assign(path).append("/").append(fileinfo.name));
					getAllFiles(p.assign(path).append("/").append(fileinfo.name), files);
				}
			}
			else
			{
				files.push_back(p.assign(path).append("/").append(fileinfo.name));
			}
		} while (_findnext(hFile, &fileinfo) == 0);  //寻找下一个，成功返回0，否则-1
		_findclose(hFile);
	}
}

/*
读取1个MPF(Multiple Pattern Feature)文件。
*/
int mpfReadFile(const string &mpfFilepath, vector<int> selectedVec, Mat &retLabel, Mat &retFeature) {
	RNG rng((unsigned)time(NULL)); //initialize RNG with the system time。
	const size_t ELEMENTCOUNT = 1;
	const size_t HEADER1_SIZE = sizeof(MPF_HEADER1);
	const size_t HEADER2_SIZE = sizeof(MPF_HEADER2);
	FILE *fp;
	if (fopen_s(&fp, mpfFilepath.c_str(), "rb+") != 0) {
		cerr << "FAIL to open input MPF file " << mpfFilepath << endl;
		return 1;
	}

	int i, j;
	vector<int>::iterator it;
	MPF_HEADER1 header1;
	MPF_HEADER2 header2;
	size_t numread = fread_s(&header1, HEADER1_SIZE, HEADER1_SIZE, ELEMENTCOUNT, fp);
	//printf("size of header:%d, format code:%s, illustration:%s, numread:%d.\n", header1.headerSize, header1.formatCode, header1.buf, numread);

	unsigned char *cPtr = header1.buf;	//illustration开始的位置。
	const unsigned char *cPtrHead = cPtr;
	while ((*cPtr) != 0)	cPtr++;
	cPtr++;	//illustration结束，下一个字段的开始位置。
	memcpy((void*)(&header2), cPtr, HEADER2_SIZE);
	//printf("illustration length:%d.\n", cPtr-cPtrHead);

	unsigned int iSampleNumber, iDimension;
	unsigned short right, left;
	right = header2.cSampleNumber[1];
	right = (right << 8) | header2.cSampleNumber[0];
	left = header2.cSampleNumber[3];
	left = (left << 8) | header2.cSampleNumber[2];
	iSampleNumber = left;
	iSampleNumber = (iSampleNumber << 16) | right;

	//计算dimension。
	right = header2.cDimension[1];
	right = (right << 8) | header2.cDimension[0];
	left = header2.cDimension[3];
	left = (left << 8) | header2.cDimension[2];
	iDimension = left;
	iDimension = (iDimension << 16) | right;
	CV_Assert(iDimension == MPF_FEATURE_DIMENSION);
	//printf("code type:%s, code length:%d, data type:%s, sample number:%d, dimension:%d, numread:%d.\n", header2.codeType, header2.codeLength, header2.dataType, iSampleNumber, iDimension, numread);

	size_t readSize;
	vector<float> featureVec;
	float fFeature;
	unsigned char featureBuf[MPF_FEATURE_SIZE];
	//需要补足第1个feature vector.
	cPtr = cPtr + HEADER2_SIZE;	//指向第1个feature的开始位置。
	it = std::find(selectedVec.begin(), selectedVec.end(), getGBFrom2Char(cPtr[0], cPtr[1]));

	Mat labelMat(1, 1, CV_32SC1);
	labelMat.ptr<int>(0)[0] = static_cast<int>(getGBFrom2Char(cPtr[0], cPtr[1]));	//保存分类结果。
	if (it != selectedVec.end())	retLabel.push_back(labelMat);	//把labelMat追加到trainLabelMat尾部。
	labelMat.release();
	//printf("第1个feature的汉字：%c%c.\n", cPtr[0], cPtr[1]);

	readSize = MPF_FEATURE_SIZE - (MPF_FEATURE_DIMENSION - (cPtr - cPtrHead));
	numread = fread_s(featureBuf, readSize, readSize, ELEMENTCOUNT, fp);
	cPtr++; cPtr++;
	for (i = 0; i != MPF_FEATURE_DIMENSION - readSize; i++) {
		fFeature = static_cast<float>(*(cPtr + i));
		fFeature = fFeature / 255.0f;	//归一化。
		featureVec.push_back(fFeature);
	}
	for (i = 0; i != readSize; i++) {
		fFeature = static_cast<float>(featureBuf[i]);
		fFeature = fFeature / 255.0f;	//归一化。
		featureVec.push_back(fFeature);
	}
	Mat tmp = Mat(featureVec, true).t();
	if (it != selectedVec.end())	retFeature.push_back(tmp);
	tmp.release();

	for (i = 0; i != iSampleNumber - 1; i++) {
		numread = fread_s(featureBuf, MPF_FEATURE_SIZE, MPF_FEATURE_SIZE, ELEMENTCOUNT, fp);
		it = std::find(selectedVec.begin(), selectedVec.end(), getGBFrom2Char(featureBuf[0], featureBuf[1]));
		if (it != selectedVec.end()) {
			Mat labelMat(1, 1, CV_32SC1);
			labelMat.ptr<int>(0)[0] = static_cast<int>(getGBFrom2Char(featureBuf[0], featureBuf[1]));	//保存分类结果。
			retLabel.push_back(labelMat);	//把labelMat追加到trainLabelMat尾部。
			labelMat.release();
			//printf("第%d个feature的汉字：%c%c.\n", i+2, featureBuf[0],featureBuf[1]);

			featureVec.clear();
			for (j = 0; j != iDimension; j++) {
				featureVec.push_back(featureBuf[2 + j]);
			}
			Mat tmp = Mat(featureVec, true).t();
			retFeature.push_back(tmp);
			tmp.release();
		}
	}

	fclose(fp);

	return 0;
}


/*
初始化2个SVM。
*/
int initClassifiers(const int classNumber) {
	LINEAR_SVM_PTR = SVM::create();
	LINEAR_SVM_PTR->setKernel(SVM::LINEAR);
	LINEAR_SVM_PTR->setType(SVM::C_SVC);	//支持向量机的类型。
	LINEAR_SVM_PTR->setC(20);
	LINEAR_SVM_PTR->setTermCriteria(TermCriteria(CV_TERMCRIT_ITER, 100, FLT_EPSILON));

	/*
	高斯核，参数效果：
	1. gamma=0.5, C=1;	错误率49%。
	2. gamma=0.5, C=20;	64%。
	3. gamma=0.5, C=80;	64%。
	4. gamma=0.5, C=160.
	5. gamma=5,	C=20.	43%。
	6. gamma=10,C=20.	58%.
	*/
	RBF_SVM_PTR = SVM::create();
	RBF_SVM_PTR->setKernel(SVM::RBF);
	RBF_SVM_PTR->setType(SVM::C_SVC);	//支持向量机的类型。
	RBF_SVM_PTR->setGamma(5);
	RBF_SVM_PTR->setC(20);	//惩罚因子。
	RBF_SVM_PTR->setTermCriteria(TermCriteria(CV_TERMCRIT_ITER, 100, FLT_EPSILON));
	cout << "Succeed to initialize 2 SVMs." << endl;

	ANN_MLP_PTR = cv::ml::ANN_MLP::create();
	const int layerSize = 3;
	/*
	共3层：输入层 + 1个隐藏层+ 1个输出层；
	输入层，神经元数量是512，和mpf数据的维度相同；
	隐藏层，神经元数量h=sqrt(512+classNumber)+3；
	输出层；神经元的个数为汉字种类的个数classNumber。
	*/
	const int h = static_cast<int>(std::sqrt(MPF_FEATURE_DIMENSION*classNumber));
	Mat layerSetting = (Mat_<int>(1, layerSize) << MPF_FEATURE_DIMENSION, h, classNumber);
	ANN_MLP_PTR->setLayerSizes(layerSetting);
	//MLP的训练方法
	ANN_MLP_PTR->setTrainMethod(ANN_MLP::BACKPROP, 0.1, 0.9);
	//激活函数
	ANN_MLP_PTR->setActivationFunction(ANN_MLP::SIGMOID_SYM);
	//迭代终止准则。
	ANN_MLP_PTR->setTermCriteria(TermCriteria(TermCriteria::MAX_ITER + TermCriteria::EPS, 300, FLT_EPSILON));

	return 0;
}

/*
计算神经网络预测结果的TopN命中。
输入参数：
realGb:预测汉字的实际GB码。
selectedVec:小批汉字GB码列表。
sortedMat:经过降序排列的神经网络输出预测矩阵。
返回参数：
top1:TOP1是否预测正确。
top5:TOP5是否预测正确。
top10:TOP10是否预测正确。
*/
int mpfTopN(const int realGb, const vector<int> &selectedVec, const Mat &sortedMat, bool *top1, bool *top5, bool *top10) {
	int top = 0, i;
	if (selectedVec[sortedMat.at<int>(0, top)] == realGb) {
		*top1 = true;
		*top5 = true;
		*top10 = true;
	}
	else {
		*top1 = false;
		*top5 = false;
		top = 1;
		for (i = top; i != 5; i++) {
			if (selectedVec[sortedMat.at<int>(0, i)] == realGb) {
				*top5 = true;
				*top10 = true;
				break;	//TOP5命中。
			}
		}
		if (*top5 == false) {
			//TOP5没有命中，需要测试TOP10是否命中。
			*top10 = false;
			top = 5;
			for (i = top; i != 10; i++) {
				if (selectedVec[sortedMat.at<int>(0, i)] == realGb) {
					*top10 = true;
					break;	//TOP10命中。
				}
			}
		}
	}


	return 0;
}

/*
从MPF文件中选择batchSize个汉字。
*/
int mpfSelectBatch(const string &mpfFilepath, const int batchSize, vector<int> &retSelectVec) {
	RNG rng((unsigned)time(NULL)); //initialize RNG with the system time。
	vector<int> gbVec;
	const size_t ELEMENTCOUNT = 1;
	const size_t HEADER1_SIZE = sizeof(MPF_HEADER1);
	const size_t HEADER2_SIZE = sizeof(MPF_HEADER2);
	FILE *fp;
	if (fopen_s(&fp, mpfFilepath.c_str(), "rb+") != 0) {
		cerr << "FAIL to open input MPF file " << mpfFilepath << endl;
		return 1;
	}

	int i, j;
	MPF_HEADER1 header1;
	MPF_HEADER2 header2;
	size_t numread = fread_s(&header1, HEADER1_SIZE, HEADER1_SIZE, ELEMENTCOUNT, fp);

	unsigned char *cPtr = header1.buf;	//illustration开始的位置。
	const unsigned char *cPtrHead = cPtr;
	while ((*cPtr) != 0)	cPtr++;
	cPtr++;	//illustration结束，下一个字段的开始位置。
	memcpy((void*)(&header2), cPtr, HEADER2_SIZE);

	unsigned int iSampleNumber, iDimension;
	unsigned short right, left;
	right = header2.cSampleNumber[1];
	right = (right << 8) | header2.cSampleNumber[0];
	left = header2.cSampleNumber[3];
	left = (left << 8) | header2.cSampleNumber[2];
	iSampleNumber = left;
	iSampleNumber = (iSampleNumber << 16) | right;

	//计算dimension。
	right = header2.cDimension[1];
	right = (right << 8) | header2.cDimension[0];
	left = header2.cDimension[3];
	left = (left << 8) | header2.cDimension[2];
	iDimension = left;
	iDimension = (iDimension << 16) | right;
	CV_Assert(iDimension == MPF_FEATURE_DIMENSION);

	size_t readSize;
	vector<float> featureVec;
	unsigned char featureBuf[MPF_FEATURE_SIZE];
	//需要补足第1个feature vector.
	cPtr = cPtr + HEADER2_SIZE;	//指向第1个feature的开始位置。
	gbVec.push_back(getGBFrom2Char(cPtr[0], cPtr[1]));

	readSize = MPF_FEATURE_SIZE - (MPF_FEATURE_DIMENSION - (cPtr - cPtrHead));
	numread = fread_s(featureBuf, readSize, readSize, ELEMENTCOUNT, fp);
	cPtr++; cPtr++;

	for (i = 0; i != iSampleNumber - 1; i++) {
		numread = fread_s(featureBuf, MPF_FEATURE_SIZE, MPF_FEATURE_SIZE, ELEMENTCOUNT, fp);
		gbVec.push_back(getGBFrom2Char(featureBuf[0], featureBuf[1]));
	}
	fclose(fp);

	for (i = 0; i != batchSize; i++) {
		j = rng.uniform(0, 3700);
		retSelectVec.push_back(gbVec[j]);
	}

	return 0;
}

/*
使用MPF特征值，进行训练和识别的基准评估。
classifierOpt:分类器选择，0:SVM; 1:ANN_BP。
*/
int mpfEvaluate(const string &mpfTrainsetDir, const string &mpfTestsetDir, const int batchSize, 
	const int classifierOpt = 0, const int pcaDimension=128) {
	vector<string> trainFiles;
	vector<int> selectedVec;
	int i, j, gb;

	//选择小批量汉字。
	mpfSelectBatch("/MLDataset/CASIA/OLHWDB1.1/mpf/train/1001.mpf", batchSize, selectedVec);
	CV_Assert(selectedVec.size() == batchSize);

	getAllFiles(mpfTrainsetDir, trainFiles);
	for (i = 0; i != trainFiles.size(); i++) {
		//注意，TRAIN_MPF_FEATURE_MAT是归一化的矩阵。
		mpfReadFile(trainFiles[i], selectedVec, TRAIN_MPF_LABEL_MAT, TRAIN_MPF_FEATURE_MAT);
	}
	vector<int>::iterator it;
	vector<int>::iterator itBegin = selectedVec.begin();
	vector<int>::iterator itEnd = selectedVec.end();

	cout << "Begin to init classifiers and train by MPF, please wait......" << endl;
	double timeStart = static_cast<double>(cv::getTickCount());
	initClassifiers(batchSize);
	PCA pca(TRAIN_MPF_FEATURE_MAT, Mat(), cv::PCA::DATA_AS_ROW, pcaDimension);//特征矩阵从512D降到128D/160D.
	PCA_FEATURE_MAT = pca.project(TRAIN_MPF_FEATURE_MAT);	//映射新空间。
	cout << "MPF train feature mat rows:" << TRAIN_MPF_FEATURE_MAT.rows << ", cols:" << TRAIN_MPF_FEATURE_MAT.cols << ", label mat rows:" << TRAIN_MPF_LABEL_MAT.rows << ", cols:" << TRAIN_MPF_LABEL_MAT.cols << endl;
	cout << "PCA train feature mat rows:" << PCA_FEATURE_MAT.rows << ", cols:" << PCA_FEATURE_MAT.cols << ", label mat rows:" << TRAIN_MPF_LABEL_MAT.rows << ", cols:" << TRAIN_MPF_LABEL_MAT.cols << endl;
	if (0 == classifierOpt) {
		cout << "Begin to train SVM......" << endl;
		LINEAR_SVM_PTR->train(PCA_FEATURE_MAT, ROW_SAMPLE, TRAIN_MPF_LABEL_MAT);
	} else {
		cout << "Begin to train ANN_BP......" << endl;

		int row, col;
		/*
		需要把TRAIN_MPF_LABEL_MAT进行BP需要的格式转换，bpLabelMat的列数等于分类数，分类的列值为1。
		比如分类有5类1~5，第1行对应的分类是3，则bpLabelMat中第1行是（0，0，1，0，0）。
		*/
		Mat bpLabelMat(TRAIN_MPF_LABEL_MAT.rows, batchSize, CV_32FC1, Scalar::all(0.0f));
		for (row = 0; row != TRAIN_MPF_LABEL_MAT.rows; row++) {
			gb = TRAIN_MPF_LABEL_MAT.at<int>(row, 0);
			it = std::find(itBegin, itEnd, gb);
			CV_Assert(it != itEnd);
			col = static_cast<int>(it - itBegin);
			bpLabelMat.at<float>(row, col) = 1.0f;
		}
		ANN_MLP_PTR->train(TRAIN_MPF_FEATURE_MAT, ROW_SAMPLE, bpLabelMat);
	}
	double duration = (cv::getTickCount() - timeStart) / cv::getTickFrequency() / 60;	//minutes.
	cout << "MPF训练时间：" << static_cast<int>(duration) << "分钟，下面开始预测，请稍候......" << endl;
	timeStart = static_cast<double>(cv::getTickCount());

	vector<string> testFiles;
	float response, Ni = 0.0f, Nc = 0.0f, Nc1 = 0.0f, Nc5 = 0.0f, Nc10 = 0.0f;
	bool isTop1, isTop5, isTop10;
	int errorCnt = 0;
//	unsigned char predictCode[2], realCode[2];
	getAllFiles(mpfTestsetDir, testFiles);
	for (i = 0; i != testFiles.size(); i++) {
		Mat featuresMat(0, MPF_FEATURE_DIMENSION, CV_32FC1);
		Mat labelsMat(0, 1, CV_32SC1);
		mpfReadFile(testFiles[i], selectedVec, labelsMat, featuresMat);
		//cout <<"test feature mat rows:"<<featuresMat.rows<<", cols:"<<featuresMat.cols<<", labelsMat rows:"<<labelsMat.rows<<", cols:"<<labelsMat.cols <<endl;
		Ni += featuresMat.rows;
		for (j = 0; j != featuresMat.rows; j++) {
			Mat featureMat(1, MPF_FEATURE_DIMENSION, CV_32FC1);
			featuresMat.row(j).copyTo(featureMat);
			if (1 == classifierOpt) {
				//使用ANN_MLP进行预测。
				Mat annResponseMat, sortedMat;
				ANN_MLP_PTR->predict(featureMat, annResponseMat);
				cv::sortIdx(annResponseMat, sortedMat, cv::SORT_EVERY_ROW + cv::SORT_DESCENDING);
				gb = labelsMat.at<int>(j, 0);	//实际的GB码。
				isTop1 = false; isTop5 = false; isTop10 = false;
				mpfTopN(gb, selectedVec, sortedMat, &isTop1, &isTop5, &isTop10);
				if (isTop1) {
					//TOP1预测正确。
					Nc1 += 1.0f;
				}
				if (isTop5) {
					//TOP5预测正确。
					Nc5 += 1.0f;
				}
				if (isTop10) {
					//TOP10预测正确。
					Nc10 += 1.0f;
				}
			} else {
				Mat pcaFeatureMat = pca.project(featureMat);
				response = LINEAR_SVM_PTR->predict(pcaFeatureMat);
				if (labelsMat.at<int>(j, 0) != static_cast<int>(response)) {
					//预测错误。
					/*
					get2CharFromInt(labelsMat.at<int>(j, 0), realCode);
					get2CharFromInt(static_cast<int>(response), predictCode);
					printf("%c%c is WRONGLY predicted to %c%c.\n", realCode[0], realCode[1], predictCode[0], predictCode[1]);
					*/
				} else {
					Nc += 1.0f;
				}
				pcaFeatureMat.release();
			}
			featureMat.release();
		}
		cout<<"第"<<i<<"/"<<testFiles.size()<<"轮预测样本数量"<<featuresMat.rows<<"个。"<<endl;
		featuresMat.release();
		labelsMat.release();
	}
	duration = (cv::getTickCount() - timeStart) / cv::getTickFrequency() / 60;	//minutes.
	cout << "MPF分类预测时间：" << static_cast<int>(duration) << "分钟，下面显示分类预测结果。" << endl;

	float CR = 0.0f, CR1 = 0.0f, CR5 = 0.0f, CR10 = 0.0f;
	if (0 == classifierOpt) {
		CR = Nc / Ni;
		cout << "OLHWDB1.1 SVM(linear)+MPF预测, Nc:" << Nc << ", Ni:" << Ni << ", CR=" << static_cast<int>(CR * 100) << "%." << endl;
	} else if (1 == classifierOpt) {
		CR1 = Nc1 / Ni;
		CR5 = Nc5 / Ni;
		CR10 = Nc10 / Ni;
		cout << "OLHWDB1.1 ANN_MLP_BP+MPF预测, Nc1:" << Nc1 << ", Ni:" << Ni << ", TOP1 CR=" << static_cast<int>(CR1 * 100) << "%." << endl;
		cout << "OLHWDB1.1 ANN_MLP_BP+MPF预测, Nc5:" << Nc5 << ", Ni:" << Ni << ", TOP5 CR=" << static_cast<int>(CR5 * 100) << "%." << endl;
		cout << "OLHWDB1.1 ANN_MLP_BP+MPF预测, Nc10:" << Nc10 << ", Ni:" << Ni << ", TOP10 CR=" << static_cast<int>(CR10 * 100) << "%." << endl;
	} else {
		cerr << "参数classifierOpt错误，导致无法预测！" << endl;
	}

	return 0;
}

/*
使用OpenCL的cv::UMat类型，进行训练和识别的基准评估。
classifierOpt:分类器选择，0:SVM; 1:ANN_BP。
*/
int oclEvaluate(const string &mpfTrainsetDir, const string &mpfTestsetDir, const int batchSize, const int classifierOpt = 0) {
	vector<string> trainFiles;
	vector<int> selectedVec;
	int i, j, gb;

	//选择小批量汉字。
	mpfSelectBatch("/MLDataset/CASIA/OLHWDB1.1/mpf/train/1001.mpf", batchSize, selectedVec);
	CV_Assert(selectedVec.size() == batchSize);

	getAllFiles(mpfTrainsetDir, trainFiles);
	for (i = 0; i != trainFiles.size(); i++) {
		//注意，TRAIN_MPF_FEATURE_MAT是归一化的矩阵。
		mpfReadFile(trainFiles[i], selectedVec, TRAIN_MPF_LABEL_MAT, TRAIN_MPF_FEATURE_MAT);
	}
	vector<int>::iterator it;
	vector<int>::iterator itBegin = selectedVec.begin();
	vector<int>::iterator itEnd = selectedVec.end();

	cout << "Begin to init classifiers and train by MPF, please wait......" << endl;
	double timeStart = static_cast<double>(cv::getTickCount());
	initClassifiers(batchSize);
	TRAIN_LABEL_UMAT	= TRAIN_MPF_LABEL_MAT.getUMat(cv::ACCESS_READ, cv::USAGE_ALLOCATE_DEVICE_MEMORY);
	TRAIN_FEATURE_UMAT	= TRAIN_MPF_FEATURE_MAT.getUMat(cv::ACCESS_READ, cv::USAGE_ALLOCATE_DEVICE_MEMORY);
	cout << "MPF train OCL_feature UMAT rows:" << TRAIN_FEATURE_UMAT.rows << ", cols:" << TRAIN_FEATURE_UMAT.cols << ", label mat rows:" << TRAIN_MPF_LABEL_MAT.rows << ", cols:" << TRAIN_MPF_LABEL_MAT.cols << endl;

	if (0 == classifierOpt) {
		cout << "Begin to train SVM......" << endl;
		LINEAR_SVM_PTR->train(TRAIN_FEATURE_UMAT, ROW_SAMPLE, TRAIN_LABEL_UMAT);
	} else {
		int row, col;
		/*
		需要把TRAIN_MPF_LABEL_MAT进行BP需要的格式转换，bpLabelMat的列数等于分类数，分类的列值为1。
		比如分类有5类1~5，第1行对应的分类是3，则bpLabelMat中第1行是（0，0，1，0，0）。
		*/
		Mat bpLabelMat(TRAIN_MPF_LABEL_MAT.rows, batchSize, CV_32FC1, Scalar::all(0.0f));
		for (row = 0; row != TRAIN_MPF_LABEL_MAT.rows; row++) {
			gb = TRAIN_MPF_LABEL_MAT.at<int>(row, 0);
			it = std::find(itBegin, itEnd, gb);
			CV_Assert(it != itEnd);
			col = static_cast<int>(it - itBegin);
			bpLabelMat.at<float>(row, col) = 1.0f;
		}
		UMat labelUMat = bpLabelMat.getUMat(cv::ACCESS_READ, cv::USAGE_ALLOCATE_DEVICE_MEMORY);
		cout << "Begin to train ANN_BP......" << endl;
		ANN_MLP_PTR->train(TRAIN_FEATURE_UMAT, cv::ml::ROW_SAMPLE, labelUMat);	//train with OCL.
	}
	double duration = (cv::getTickCount() - timeStart) / cv::getTickFrequency() / 60;	//minutes.
	cout << "oclMPF训练时间：" << static_cast<int>(duration) << "分钟，下面开始预测，请稍候。" << endl;
	timeStart = static_cast<double>(cv::getTickCount());

	vector<string> testFiles;
	float response, Ni = 0.0f, Nc = 0.0f, Nc1 = 0.0f, Nc5 = 0.0f, Nc10 = 0.0f;
	bool isTop1, isTop5, isTop10;
	int errorCnt = 0;
//	unsigned char predictCode[2], realCode[2];
	getAllFiles(mpfTestsetDir, testFiles);
	for (i = 0; i != testFiles.size(); i++) {
		Mat featuresMat(0, MPF_FEATURE_DIMENSION, CV_32FC1);
		Mat labelsMat(0, 1, CV_32SC1);
		mpfReadFile(testFiles[i], selectedVec, labelsMat, featuresMat);
		//cout <<"test feature mat rows:"<<featuresMat.rows<<", cols:"<<featuresMat.cols<<", labelsMat rows:"<<labelsMat.rows<<", cols:"<<labelsMat.cols <<endl;
		Ni += featuresMat.rows;
		for (j = 0; j != featuresMat.rows; j++) {
			//Mat featureMat(1, MPF_FEATURE_DIMENSION, CV_32FC1);
			Mat featureUMat(1, MPF_FEATURE_DIMENSION, CV_32FC1);
			featuresMat.row(j).copyTo(featureUMat);
			if (1 == classifierOpt) {
				//使用ANN_MLP进行预测。
				//Mat annResponseMat, sortedMat;
				cv::UMat responseUMat, sortedUMat;
				ANN_MLP_PTR->predict(featureUMat, responseUMat);//Predict with OCL.

				cv::sortIdx(responseUMat, sortedUMat, cv::SORT_EVERY_ROW + cv::SORT_DESCENDING);
				gb = labelsMat.at<int>(j, 0);	//实际的GB码。
				isTop1 = false; isTop5 = false; isTop10 = false;
				Mat sortedMat	= sortedUMat.getMat(ACCESS_READ);
				mpfTopN(gb, selectedVec, sortedMat, &isTop1, &isTop5, &isTop10);
				if (isTop1) {
					//TOP1预测正确。
					Nc1 += 1.0f;
				}
				if (isTop5) {
					//TOP5预测正确。
					Nc5 += 1.0f;
				}
				if (isTop10) {
					//TOP10预测正确。
					Nc10 += 1.0f;
				}
			} else {
				response = LINEAR_SVM_PTR->predict(featureUMat);

				if (labelsMat.at<int>(j, 0) != static_cast<int>(response)) {
					//预测错误。
					/*
					get2CharFromInt(labelsMat.at<int>(j, 0), realCode);
					get2CharFromInt(static_cast<int>(response), predictCode);
					printf("%c%c is WRONGLY predicted to %c%c.\n", realCode[0], realCode[1], predictCode[0], predictCode[1]);
					*/
				}
				else {
					Nc += 1.0f;
				}
			}
			featureUMat.release();
		}
		featuresMat.release();
		labelsMat.release();
	}
	duration = (cv::getTickCount() - timeStart) / cv::getTickFrequency() / 60;	//minutes.
	cout << "MPF分类预测时间：" << static_cast<int>(duration) << "分钟，下面显示分类预测结果。" << endl;

	float CR = 0.0f, CR1 = 0.0f, CR5 = 0.0f, CR10 = 0.0f;
	if (0 == classifierOpt) {
		CR = Nc / Ni;
		cout << "OLHWDB1.1 SVM(linear)+MPF预测, Nc:" << Nc << ", Ni:" << Ni << ", CR=" << static_cast<int>(CR * 100) << "%." << endl;
	}
	else if (1 == classifierOpt) {
		CR1 = Nc1 / Ni;
		CR5 = Nc5 / Ni;
		CR10 = Nc10 / Ni;
		cout << "OLHWDB1.1 ANN_MLP_BP+MPF预测, Nc1:" << Nc1 << ", Ni:" << Ni << ", TOP1 CR=" << static_cast<int>(CR1 * 100) << "%." << endl;
		cout << "OLHWDB1.1 ANN_MLP_BP+MPF预测, Nc5:" << Nc5 << ", Ni:" << Ni << ", TOP5 CR=" << static_cast<int>(CR5 * 100) << "%." << endl;
		cout << "OLHWDB1.1 ANN_MLP_BP+MPF预测, Nc10:" << Nc10 << ", Ni:" << Ni << ", TOP10 CR=" << static_cast<int>(CR10 * 100) << "%." << endl;
	}
	else {
		cerr << "参数classifierOpt错误，导致无法预测！" << endl;
	}

	return 0;
}

int showOcl() {
	if (!cv::ocl::haveOpenCL())
	{
		cout << "OpenCL is not avaiable..." << endl;
		return 1;
	}
	cv::ocl::Context allContext;
	if (!allContext.create(cv::ocl::Device::TYPE_ALL))
	{
		cout << "Failed creating the all context..." << endl;
		return 1;
	}
	cout << allContext.ndevices() << " all of OpenCL devices are detected." << endl;
	for (int i = 0; i < allContext.ndevices(); i++)
	{
		cv::ocl::Device device = allContext.device(i);
		cout << "name                 : " << device.name() << endl;
		cout << "available            : " << device.available() << endl;
		cout << "imageSupport         : " << device.imageSupport() << endl;
		cout << "OpenCL_C_Version     : " << device.OpenCL_C_Version() << endl;
		cout << endl;
	}

	cv::ocl::Context gpuContext;
	if (!gpuContext.create(cv::ocl::Device::TYPE_GPU))
	{
		cout << "Failed creating the GPU context..." << endl;
		return 1;
	}

	// In OpenCV 3.4.x, only a single device is detected.
	cout << gpuContext.ndevices() << " GPU devices are detected." << endl;
	for (int i = 0; i < gpuContext.ndevices(); i++)
	{
		cv::ocl::Device device = gpuContext.device(i);
		cout << "name                 : " << device.name() << endl;
		cout << "available            : " << device.available() << endl;
		cout << "imageSupport         : " << device.imageSupport() << endl;
		cout << "OpenCL_C_Version     : " << device.OpenCL_C_Version() << endl;
		cout << endl;
	}

	return 0;
}


int main(int argc, char *argv[])
{
	const String keys =
		"{help h usage ? |      | print this message   }"
		"{mpfpath        |C:/MLDataset/CASIA/OLHWDB1.1/mpf | MPF文件路径，该路径下应该有train和test子目录，里面保存了训练集和测试集的MPF文件 }"
		"{batchsize      |100   | 小批训练预测的汉字数量 }"
		"{classifier     |SVM   | 选择使用哪个分类器进行训练预测，可以使用ANN或者SVM分类器 }"
		"{pca            |128   | 使用PCA的维数（原始维数512D）,可选128或160 }"
		"{showocl        |      | 显示当前OpenCL配置	 }"
		"{ocl            |      | 是否使用OpenCL显卡加速进行训练 }"
		;//第2列是缺省值。

	CommandLineParser parser(argc, argv, keys);
	parser.about("ICDAR2013任务1");
	if (parser.has("help"))
	{
		parser.printMessage();
		return 0;
	}
	if (parser.has("showocl"))
	{
		showOcl();
		return 0;
	}

	const cv::String mpfPath= parser.get<String>("mpfpath");
	const int batchsize	= parser.get<int>("batchsize");
	const cv::String classifier = parser.get<String>("classifier");
	const int pcaDimension	= parser.get<int>("pca");
	const bool isOcl	= parser.has("ocl");
	if (pcaDimension != 160 && pcaDimension != 128) {
		parser.printErrors();
		return 1;
	}

	int classifierOpt = 0;
	if (classifier == "ANN")	classifierOpt = 1;
	std::string mpfDir(mpfPath.c_str());
	MPF_TRAINSET_DIR = mpfDir + "/train";
	MPF_TESTSET_DIR = mpfDir + "/test";

	cout << "你选择了" << batchsize << "个汉字进行训练和分类预测，请等待......" << endl;
	if (0 == classifierOpt) {
		cout << "你选择了SVM分类器，请等待......" << endl;
	} else {
		cout << "你选择了ANN人工神经网络分类器，请等待......" << endl;
	}
	
	cout << "你选择了PCA降维到"<<pcaDimension<<"D."<<endl;

	if (isOcl) {
		cout << "你选择了使用OpenCL进行显卡加速训练和预测，请等待......" << endl;
		cv::ocl::setUseOpenCL(true);
		oclEvaluate(MPF_TRAINSET_DIR, MPF_TESTSET_DIR, batchsize, classifierOpt);
	} else {
		cout << "你没有使用OpenCL进行显卡加速训练和预测，请等待......" << endl;
		mpfEvaluate(MPF_TRAINSET_DIR, MPF_TESTSET_DIR, batchsize, classifierOpt, pcaDimension);
	}

	return 0;
}

