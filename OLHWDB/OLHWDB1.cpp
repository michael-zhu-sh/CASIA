/*
Author:Michael Zhu.
QQ:1265390626.
Email:1265390626@qq.com
QQ群:279441740
GIT:https://github.com/michael-zhu-sh/
本项目使用OpenCV提供的HOG特征和SVM分类器，
在CASIA OLHWDB手写汉字数据集上，进行了训练，完成了2013年中文手写识别大赛的第3个任务。
本项目的在线手写汉字训练数据集下载链接http://www.nlpr.ia.ac.cn/databases/download/feature_data/OLHWDB1.1trn_pot.zip
测试数据集下载链接http://www.nlpr.ia.ac.cn/databases/download/feature_data/OLHWDB1.1tst_pot.zip
大赛ICDAR2013官网http://www.nlpr.ia.ac.cn/events/CHRcompetition2013/competition/Home.html
大赛测试数据集下载链接http://www.nlpr.ia.ac.cn/databases/Download/competition/competition_POT.zip
*/
#include "stdafx.h"

#include <io.h>
#include <stdio.h>

#include <opencv2/opencv.hpp>

using namespace std;
using namespace cv;

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
const string TRAINSET_FILEPATH = "";	//训练集目录。
const string TESTSET_FILEPATH = "";		//测试集目录。
const string ICDAR2013_FILEPATH = "";	//大赛测试集目录。

/*---------------------------------------------------------------------------*/
int getGBFrom2Char(unsigned char code1, unsigned char code2) {
	unsigned short tag, ltag, rtag;
	tag = code1;
	ltag = tag << 8;
	rtag = code2;
	return (ltag | rtag);
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
读取1个POT文件中的所有笔画汉字样本。
返回：
retClassSampleMap:存放样本的GB码和样本数量的映射, key is GB code, value is number of samples。
*/
int readAPot(const string &potFilepath, map<int,int> &retClassSampleMap) {
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
		cout << "Succeed to open input POT file " << potFilepath <<endl;
	}

	while (true) {
		numread = fread_s(&header, HEADER_SIZE, HEADER_SIZE, ELEMENTCOUNT, fp);
		if (numread != ELEMENTCOUNT) {
			cerr << "FAIL to read POT_HEADER!" << endl;
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
		//Mat binImg = getImageFromStroke(strokeVec);	//图像化这个笔画汉字（二值图像）。
		pageNo = sampleAmt / SAMPLES_IN_A_PAGE + 1;
		pageSampleNo = sampleAmt - (pageNo-1) * SAMPLES_IN_A_PAGE + 1;
		printf("Page %d sample %d code %c%c.\n", pageNo, pageSampleNo, header.tagCode[1], header.tagCode[0]);	//汉字GB码。
		gb = getGBFrom2Char(header.tagCode[1], header.tagCode[0]);
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
	cout << "已成功读入"<<sampleAmt<<"个汉字样本。"<<endl;

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
*/
int explore(int *retData) {
	vector<string> trainFiles,testFiles,icdarFiles;
	getAllFiles(TRAINSET_FILEPATH, trainFiles);
	getAllFiles(TESTSET_FILEPATH, testFiles);
	getAllFiles(ICDAR2013_FILEPATH, icdarFiles);

	int i,sampleAmt=0;
	map<int, int> labelMap;
	map<int, int>::iterator it;
	int numberOfPOTs	= trainFiles.size();
	for (i = 0; i != numberOfPOTs; i++) {
		readAPot(trainFiles[i], labelMap);
	}	//读取训练集所有的POT文件。
	retData[0] = numberOfPOTs;	//训练集POT文件总数量。
	for (it = labelMap.begin(); it != labelMap.end(); it++) {
		sampleAmt += it->second;
	}
	retData[1] = sampleAmt;	//训练集手写汉字样本总数量。
	retData[2] = labelMap.size();	//训练集手写汉字类别总数量。

	labelMap.clear();
	sampleAmt	= 0;
	numberOfPOTs= testFiles.size();
	for (i = 0; i != numberOfPOTs; i++) {
		readAPot(testFiles[i], labelMap);
	}	//读取测试集所有的POT文件。
	retData[3] = numberOfPOTs;	//测试集POT文件总数量。
	for (it = labelMap.begin(); it != labelMap.end(); it++) {
		sampleAmt += it->second;
	}
	retData[4] = sampleAmt;	//测试集手写汉字样本总数量。
	retData[5] = labelMap.size();	//测试集手写汉字类别总数量。

	labelMap.clear();
	sampleAmt = 0;
	numberOfPOTs = icdarFiles.size();
	for (i = 0; i != numberOfPOTs; i++) {
		readAPot(icdarFiles[i], labelMap);
	}	//ICDAR测试集所有的POT文件。
	retData[6] = numberOfPOTs;	//icdar测试集POT文件总数量。
	for (it = labelMap.begin(); it != labelMap.end(); it++) {
		sampleAmt += it->second;
	}
	retData[7] = sampleAmt;	//ICDAR测试集手写汉字样本总数量。
	retData[8] = labelMap.size();	//ICDAR测试集手写汉字类别总数量。

	cout << "训练集POT文件总数量:"<<retData[0]<<endl;
	cout << "训练集手写汉字样本总数量:" << retData[1] << endl;
	cout << "训练集手写汉字类别总数量:" << retData[2] << endl;
	cout << "测试集POT文件总数量:" << retData[3] << endl;
	cout << "测试集手写汉字样本总数量:" << retData[4] << endl;
	cout << "测试集手写汉字类别总数量:" << retData[5] << endl;
	cout << "ICDAR2013测试集POT文件总数量:" << retData[6] << endl;
	cout << "ICDAR2013测试集手写汉字样本总数量:" << retData[7] << endl;
	cout << "ICDAR2013测试集手写汉字类别总数量:" << retData[8] << endl;

	return 0;
}

int main()
{
	int statistics[10];
	explore(statistics);

    return 0;
}

