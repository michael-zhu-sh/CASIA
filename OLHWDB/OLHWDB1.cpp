/*
Author:Michael Zhu.
QQ:1265390626.
Email:1265390626@qq.com
QQ群:279441740
GIT:https://github.com/michael-zhu-sh/CASIA
本项目使用OpenCV提供的HOG特征和SVM分类器，
在CASIA OLHWDB手写汉字数据集上，进行了训练，完成了2013年中文手写识别大赛的第3个任务。
本项目的在线手写汉字训练数据集下载链接http://www.nlpr.ia.ac.cn/databases/download/feature_data/OLHWDB1.1trn_pot.zip
测试数据集下载链接http://www.nlpr.ia.ac.cn/databases/download/feature_data/OLHWDB1.1tst_pot.zip
ICDAR2013大赛官网http://www.nlpr.ia.ac.cn/events/CHRcompetition2013/competition/Home.html
大赛测试数据集下载链接http://www.nlpr.ia.ac.cn/databases/Download/competition/competition_POT.zip
*/
#include "stdafx.h"

#include <io.h>
#include <stdio.h>

#include <opencv2/opencv.hpp>

using namespace std;
using namespace cv;
using namespace cv::ml;


//POT文件中每个汉字的头信息。
struct POT_HEADER {
	unsigned short sampleSize;
	unsigned char tagCode[4];	//[1][0]是正确的汉字GB码。
	unsigned short strokeNumber;	//该汉字的笔画数量。
};
//笔画中的点信息。
struct COORDINATE {
	signed short x;
	signed short y;
};
//一个汉字的结束标记。
struct END_TAG {
	signed short endflag0;
	signed short endflag1;
};
//一个笔画结束的坐标。
const signed short STROKE_END_X = -1;
const signed short STROKE_END_Y = 0;
const size_t SAMPLES_IN_A_PAGE = 80;	//POT中每页的样本数量。
const string TRAINSET_DIR	= "/MLDataset/CASIA/OLHWDB1.1/train";	//训练集目录。
const string TESTSET_DIR	= "/MLDataset/CASIA/OLHWDB1.1/test";		//测试集目录。
const string ICDAR2013_DIR	= "/MLDataset/CASIA/OLHWDB1.1/competition";	//大赛测试集目录。
/*全局HOG参数--------------------*/
const cv::Size WIN_SIZE(64, 64);	//原始汉字从文件读入后，需要尺度转换到这里64cols64rows。
const cv::Size BLOCK_SIZE(16, 16);
const cv::Size BLOCK_STRIDE(8, 8);
const cv::Size CELL_SIZE(8, 8);
const int N_BINS = 9;
const int FEATURE_DIMENSION = 1764;
/*全局HOG参数--------------------*/
Mat TRAIN_FEATURE_MAT(0, FEATURE_DIMENSION, CV_32FC1);//存放训练集所有的HOG特征向量。
Mat TRAIN_LABEL_MAT(0, 1, CV_32SC1);

cv::Ptr<SVM> LINEAR_SVM_PTR;
cv::Ptr<SVM> RBF_SVM_PTR;



/*---------------------------------------------------------------------------*/
int getGBFrom2Char(unsigned char code1, unsigned char code2) {
	unsigned short tag, ltag, rtag;
	tag = code1;
	ltag = tag << 8;
	rtag = code2;
	return (ltag | rtag);
}
int get2CharFromInt(const int label, unsigned char *gb) {
	unsigned short tag;
	tag = static_cast<unsigned short>(label);
	gb[0] = static_cast<unsigned char>(tag >> 8);
	gb[1] = static_cast<unsigned char>((tag << 8) >> 8);

	return 0;
}
/*
判断汉字gb是否属于小批。
*/
bool isMiniBatch(const int gb, const int miniBatchSize, const int *batchLabelPtr) {
	for (int i = 0; i != miniBatchSize; i++) {
		if (gb == batchLabelPtr[i])	return true;
	}

	return false;
}

/*
根据汉字样本笔画的坐标序列画出图像。
返回：二值化汉字图像。
*/
Mat getImageFromStroke(const vector<vector<COORDINATE>> &strokeVec) {
	const int LINE_THICKNESS = 2;
	const size_t PAD_SIZE = 4;
	int minx = 32767, miny = 32767, maxx=0,maxy=0;
	int matWidth,matHeight;
	size_t strokeSize = strokeVec.size();
	size_t i,j,pointSize;
	vector<COORDINATE > points;
	for (i = 0; i != strokeSize; i++) {
		points = strokeVec[i];
		pointSize = points.size();
		for (j = 0; j != pointSize; j++) {
			if (points[j].x < minx) {
				minx = points[j].x;
			}
			if (points[j].y < miny) {
				miny = points[j].y;
			}
			if (points[j].x > maxx) {
				maxx = points[j].x;
			}
			if (points[j].y > maxy) {
				maxy = points[j].y;
			}
		}
	}
	matWidth	= maxx - minx + PAD_SIZE * 2;
	matHeight	= maxy - miny + PAD_SIZE * 2;
	Mat image(matHeight, matWidth, CV_8UC1, Scalar(0));
	cv::Point from, to;
	for (i = 0; i != strokeSize; i++) {
		points = strokeVec[i];
		pointSize = points.size();
		if (1 == pointSize)	continue;	//只有1个点，不画line。
		for (j = 0; j != pointSize; j++) {
			from.x = points[j].x - minx + PAD_SIZE;
			from.y = points[j].y - miny + PAD_SIZE;
			to.x = points[j+1].x - minx + PAD_SIZE;
			to.y = points[j+1].y - miny + PAD_SIZE;
			cv::line(image, from, to, Scalar(255), LINE_THICKNESS);
			if (j==pointSize-2)	break;
		}
	}

	return image;
}

/*
读取1个POT文件中的所有笔画汉字样本并转化为图像汉字，各种图像预处理可在本函数中完成。
返回：
retClassSampleMap:存放样本的GB码和样本数量的映射, key is GB code, value is number of samples。
*/
int readAPot(const string &potFilepath, map<int,int> &retClassSampleMap, vector<Mat> &retImages, vector<int> &retLabels,
			const int miniBatchSize=0, const int *batchLabelPtr=NULL) {
	const size_t COORDINATE_SIZE = sizeof(COORDINATE);
	const size_t ELEMENTCOUNT = 1;
	const size_t HEADER_SIZE= sizeof(POT_HEADER);
	const size_t END_SIZE = sizeof(END_TAG);
	POT_HEADER header;
	COORDINATE coordinate;
	END_TAG endtag;
	size_t numread;
	size_t strokeCnt=0;
	int pageNo,pageSampleNo,sampleAmt=0,gb,numberOfSamples;
	vector<vector<COORDINATE>> strokeVec;
	map<int, int>::iterator it;

	FILE *fp;
	if (fopen_s(&fp, potFilepath.c_str(), "rb+") != 0) {
		cerr << "FAIL to open input POT file " << potFilepath << endl;
		return 1;
	} else {
		//cout << "Succeed to open input POT file " << potFilepath <<endl;
	}

	while (true) {
		numread = fread_s(&header, HEADER_SIZE, HEADER_SIZE, ELEMENTCOUNT, fp);
		if (numread != ELEMENTCOUNT) {
			//cerr << "FAIL to read POT_HEADER!" << endl;
			break;
		}
		vector<COORDINATE> points;
		for (strokeCnt = 0; strokeCnt != header.strokeNumber; strokeCnt++) {
			while (true) {
				numread = fread_s(&coordinate, COORDINATE_SIZE, COORDINATE_SIZE, ELEMENTCOUNT, fp);	//读取笔画里面的点坐标。
				if (STROKE_END_X==coordinate.x && STROKE_END_Y==coordinate.y) {
					//该笔画结束。
					break;
				} else {
					points.push_back(coordinate);
				}
			}	//读取1笔画结束。
			strokeVec.push_back(points);
			points.clear();
		}//读取字符结束。
		gb = getGBFrom2Char(header.tagCode[1], header.tagCode[0]);
		if (0!=miniBatchSize && isMiniBatch(gb, miniBatchSize, batchLabelPtr)) {
			//该汉字在小批中，需要提取图像。
			Mat binImg = getImageFromStroke(strokeVec);	//图像化这个笔画汉字（二值图像）。
			Mat resizeImg;
			cv::resize(binImg, resizeImg, WIN_SIZE);	//尺度变换到HOG计算。
			//其他图像预处理方法可在此完成TODO。

			retImages.push_back(resizeImg);
			retLabels.push_back(gb);
			resizeImg.release();
			binImg.release();
		}
		pageNo = sampleAmt / SAMPLES_IN_A_PAGE + 1;
		pageSampleNo = sampleAmt - (pageNo-1) * SAMPLES_IN_A_PAGE + 1;
		//printf("Page %d sample %d code %c%c.\n", pageNo, pageSampleNo, header.tagCode[1], header.tagCode[0]);	//汉字GB码。
		it	= retClassSampleMap.find(gb);
		if (it != retClassSampleMap.end()) {
			//found.
			numberOfSamples = it->second;
			numberOfSamples++;
			retClassSampleMap.erase(it);
			retClassSampleMap.insert(pair<int, int>(gb, numberOfSamples));
		} else {
			retClassSampleMap.insert(pair<int, int>(gb, 1));
		}
		sampleAmt++;	//这个POT文件里的汉字样本总数量。

		strokeVec.clear();
		fread_s(&endtag, END_SIZE, END_SIZE, ELEMENTCOUNT, fp);	//读取字符结束标志。
	}
	fclose(fp);
	//cout << "已成功读入"<<sampleAmt<<"个汉字样本。"<<endl;

	return 0;
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
分析数据集的统计信息。
返回：
retStatDataPtr:存放计算得到的数据集的统计数据值。
retBatchLabelPtr:存放从大赛测试集中随机抽取的汉字GB码。
*/
int explore(const string &trainsetDir, const string &testsetDir, const string &competitionDir,
			int *retStatDataPtr,
			const int miniBatchSize, int *retBatchLabelPtr) {
	
	RNG rng((unsigned)time(NULL)); // initialize with the system time
	vector<string> trainFiles,testFiles,icdarFiles;
	getAllFiles(trainsetDir, trainFiles);
	getAllFiles(testsetDir, testFiles);
	getAllFiles(competitionDir, icdarFiles);

	int i,j,sampleAmt=0;
	map<int, int> labelMap;
	map<int, int>::iterator it;
	vector<Mat> images;
	vector<int> labels;
	int numberOfPOTs	= trainFiles.size();
	for (i = 0; i != numberOfPOTs; i++) {
		//cout << i << "/" << numberOfPOTs << " begin to process " << trainFiles[i] << endl;
		readAPot(trainFiles[i], labelMap, images, labels);
	}	//读取训练集所有的POT文件。
	retStatDataPtr[0] = numberOfPOTs;	//训练集POT文件总数量。
	for (it = labelMap.begin(); it != labelMap.end(); it++) {
		sampleAmt += it->second;
	}
	retStatDataPtr[1] = sampleAmt;	//训练集手写汉字样本总数量。
	retStatDataPtr[2] = labelMap.size();	//训练集手写汉字类别总数量。

	labelMap.clear();
	sampleAmt	= 0;
	numberOfPOTs= testFiles.size();
	for (i = 0; i != numberOfPOTs; i++) {
		//cout <<i<< "/"<<numberOfPOTs<<" begin to process "<<testFiles[i] << endl;
		readAPot(testFiles[i], labelMap, images, labels);
	}	//读取测试集所有的POT文件。
	retStatDataPtr[3] = numberOfPOTs;	//测试集POT文件总数量。
	for (it = labelMap.begin(); it != labelMap.end(); it++) {
		sampleAmt += it->second;
	}
	retStatDataPtr[4] = sampleAmt;	//测试集手写汉字样本总数量。
	retStatDataPtr[5] = labelMap.size();	//测试集手写汉字类别总数量。

	vector<int> mapLabelVec;
	labelMap.clear();
	sampleAmt = 0;
	numberOfPOTs = icdarFiles.size();
	for (i = 0; i != numberOfPOTs; i++) {
		//cout << i << "/" << numberOfPOTs << " begin to process " << icdarFiles[i] << endl;
		readAPot(icdarFiles[i], labelMap, images, labels);
	}	//ICDAR测试集所有的POT文件。
	retStatDataPtr[6] = numberOfPOTs;	//icdar测试集POT文件总数量。
	for (it = labelMap.begin(); it != labelMap.end(); it++) {
		mapLabelVec.push_back(it->first);
		sampleAmt += it->second;
	}
	retStatDataPtr[7] = sampleAmt;	//ICDAR测试集手写汉字样本总数量。
	retStatDataPtr[8] = labelMap.size();	//ICDAR测试集手写汉字类别总数量。

	cout << "训练集POT文件总数量:"<< retStatDataPtr[0]<<endl;
	cout << "训练集手写汉字样本总数量:" << retStatDataPtr[1] << endl;
	cout << "训练集手写汉字类别总数量:" << retStatDataPtr[2] << endl;
	cout << "测试集POT文件总数量:" << retStatDataPtr[3] << endl;
	cout << "测试集手写汉字样本总数量:" << retStatDataPtr[4] << endl;
	cout << "测试集手写汉字类别总数量:" << retStatDataPtr[5] << endl;
	cout << "ICDAR2013测试集POT文件总数量:" << retStatDataPtr[6] << endl;
	cout << "ICDAR2013测试集手写汉字样本总数量:" << retStatDataPtr[7] << endl;
	cout << "ICDAR2013测试集手写汉字类别总数量:" << retStatDataPtr[8] << endl;

	for (i = 0; i != miniBatchSize; i++) {
		j = rng.uniform(0,3755);
		retBatchLabelPtr[i] = mapLabelVec[j];
	}

	return 0;
}

/*
计算images中汉字图像的HOG特征。
*/
int computeHog(const vector<Mat> &images, Mat &trainFeatureMat) {
	const size_t imgCnt = images.size();
	cv::HOGDescriptor hog(WIN_SIZE, BLOCK_SIZE, BLOCK_STRIDE, CELL_SIZE, N_BINS);
	vector<float> descriptors;
	for (int i = 0; i != imgCnt; i++) {
		hog.compute(images[i], descriptors, Size(1, 1));
		Mat tmp = Mat(descriptors, true).t();
		tmp.row(0).copyTo(trainFeatureMat.row(i));	//保存HOG特征向量。
		tmp.release();
		descriptors.clear();
	}

	return 0;
}
int computeAHog(const Mat &image, Mat &featureMat) {
	cv::HOGDescriptor hog(WIN_SIZE, BLOCK_SIZE, BLOCK_STRIDE, CELL_SIZE, N_BINS);
	vector<float> descriptors;
	hog.compute(image, descriptors, Size(1, 1));
	Mat tmp = Mat(descriptors, true).t();
	tmp.row(0).copyTo(featureMat.row(0));	//保存HOG特征向量。

	return 0;
}

/*
初始化2个SVM。
*/
int initSVM() {
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

	return 0;
}

/*
根据小批指定的汉字GB码，从所有POT中提取它们的图像，计算它们的HOG特征，进行SVM训练。
*/
int miniBatchTrain(const string &trainsetDir, const int miniBatchSize, const int *batchLabelPtr) {
	vector<string> potTrainFiles;
	vector<Mat> images;
	vector<int> labels;
	map<int, int> labelSampleMap;
	int i, j, imgSize, sampleAmt=0;
	getAllFiles(trainsetDir, potTrainFiles);
	for (i = 0; i != potTrainFiles.size(); i++) {
		readAPot(potTrainFiles[i], labelSampleMap, images, labels, miniBatchSize, batchLabelPtr);
		imgSize = images.size();
		sampleAmt += imgSize;
		Mat featureMat(imgSize, FEATURE_DIMENSION, CV_32FC1);	//1个image1行特征。
		computeHog(images, featureMat);
		//CV_Assert(featureMat.rows == subTrainSize && featureMat.cols == FEATURE_VECTOR_SIZE);
		TRAIN_FEATURE_MAT.push_back(featureMat);	//把featureMat追加到trainFeatureMat尾部。
		featureMat.release();

		Mat labelMat(imgSize, 1, CV_32SC1);
		for (j = 0; j != imgSize; j++) {
			labelMat.ptr<int>(j)[0] = static_cast<int>(labels[j]);	//保存分类结果。
		}
		TRAIN_LABEL_MAT.push_back(labelMat);	//把labelMat追加到trainLabelMat尾部。
		labelMat.release();

		images.clear();
		labels.clear();

		//cout << "trainFeatureMat rows:" << TRAIN_FEATURE_MAT.rows << ", cols:" << TRAIN_FEATURE_MAT.cols << endl;
	}
	
	cout << "Begin to init SVM and train, please wait......" << endl;
	double timeStart = static_cast<double>(cv::getTickCount());
	initSVM();
	LINEAR_SVM_PTR->train(TRAIN_FEATURE_MAT, ROW_SAMPLE, TRAIN_LABEL_MAT);
	//linearSvmPtr->save("output/SVM_minibatch.yml");
	double duration = (cv::getTickCount() - timeStart) / cv::getTickFrequency() / 60;	//minutes.
	cout <<"选择的汉字数量："<<miniBatchSize<<"，参加训练的样本数量："<<sampleAmt<<"，训练时间："<<static_cast<int>(duration)<<"分钟。"<<endl;

	return 0;
}

/*
从所有测试集中选择指定的汉字进行预测。
*/
int miniBatchPredict(const string &predictDir, const int batchSize, const int *batchLabelPtr) {
	vector<string> predictFiles;
	vector<Mat> images;
	vector<int> labels;
	map<int, int> labelSampleMap;
	int i, j, imgSize, errorCnt = 0;
	float response,Ni=0.0f,Nc=0.0f;
	unsigned char realCode[2], predictCode[2];
	
	double timeStart = static_cast<double>(cv::getTickCount());
	getAllFiles(predictDir, predictFiles);
	//cout << "OLHWDB1.1预测POT文件总数：" << predictFiles.size() << endl;
	for (i = 0; i != predictFiles.size(); i++) {
		readAPot(predictFiles[i], labelSampleMap, images, labels, batchSize, batchLabelPtr);
		imgSize = images.size();
		Ni += imgSize;
		for (j = 0; j != imgSize; j++) {
			Mat featureMat(1, FEATURE_DIMENSION, CV_32FC1);	//1个image1行特征。
			computeAHog(images[j], featureMat);
			response = LINEAR_SVM_PTR->predict(featureMat);
			if (labels[j] != static_cast<int>(response)) {
				//预测错误。
				errorCnt++;
				get2CharFromInt(labels[j], realCode);
				get2CharFromInt(static_cast<int>(response), predictCode);
				//printf("%c%c is WRONGLY predicted to %c%c.\n", realCode[0], realCode[1], predictCode[0], predictCode[1]);
			} else {
				Nc += 1.0f;
			}
		}
	}
	double duration = (cv::getTickCount() - timeStart) / cv::getTickFrequency() / 60;	//minutes.

	float CR = Nc / Ni;
	cout << "OLHWDB1.1 SVM(linear)+HOG预测, POT files directory:"<<predictDir<<"，选择汉字数量：" << batchSize<<"，选择样本数量："<<static_cast<int>(Ni)<<"，CR="<< static_cast<int>(CR*100)<<"%，耗时："<< static_cast<int>(duration)<<" 分钟。"<<endl;

	return 0;
}

int miniBatch(const int miniBatchSize, const int *batchLabelPtr) {
	miniBatchTrain(TRAINSET_DIR,	miniBatchSize, batchLabelPtr);
	
	miniBatchPredict(TESTSET_DIR,	miniBatchSize, batchLabelPtr);

	miniBatchPredict(ICDAR2013_DIR, miniBatchSize, batchLabelPtr);

	return 0;
}


int main(int argc, char* argv[])
{
	if (argc!=3) {
		cerr << "输入参数错误，正确的命令行可以如下：OLHWDB1 -batchsize 50，该命令行表示从GB2312-80 1级汉字中任意选择50个汉字进行训练和分类预测。"<<endl;
		return 1;
	}
	stringstream ss;
	int batchSize = 10;
	ss << argv[2];
	ss >> batchSize;
	cout << "你选择了"<<batchSize<<"个汉字进行训练和分类预测，请等待......"<<endl;

	int statistics[10];	//数据集统计结果。
	int batchLabel[3755];//miniBatch包含的汉字GB码。

	//扫描完成的数据集，获取各类统计值。
	explore(TRAINSET_DIR, TESTSET_DIR, ICDAR2013_DIR, statistics, batchSize, batchLabel);

	miniBatch(batchSize, batchLabel);

    return 0;
}

