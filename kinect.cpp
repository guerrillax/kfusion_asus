#include <GL/glew.h>

#include "kfusion.h"
#include "helpers.h"

#include <iostream>
#include <sstream>
#include <iomanip>


#ifdef __APPLE__
#include <GLUT/glut.h>
#else
#include <GL/glut.h>
#endif

#include "perfstats.h"

using namespace std;
using namespace TooN;

#include <libfreenect/libfreenect.h>

#include <pangolin/pangolin.h>
#include <pangolin/video.h>


pangolin::VideoInput * video;
uint16_t * depth_ptr;
uint8_t * img;



int InitKinect( uint16_t * buffer ){
  depth_ptr = buffer;

  std::string uri = "openni:[img1=depth]//0";
  video = new pangolin::VideoInput(uri);

  img = new unsigned char[video->SizeBytes()];
  
  return 0;
}

void CloseKinect(){

}

void DepthFrameKinect() {
  

  video->GrabNext(img,true);
  memcpy(depth_ptr, img, video->SizeBytes());


}

KFusion kfusion;
Image<uchar4, HostDevice> lightScene, depth, lightModel;
Image<uint16_t, HostDevice> depthImage;

float3 translation = make_float3(1.0, -2, 1.0);
const float3 ambient = make_float3(0.1, 0.1, 0.1);

SE3<float> initPose;

int counter = 0;
bool reset = true;

void display(void){
  const uint2 imageSize = kfusion.configuration.inputSize;
  static bool integrate = true;

  glClear( GL_COLOR_BUFFER_BIT );
  const double startFrame = Stats.start();

  DepthFrameKinect();
  const double startProcessing = Stats.sample("kinect");

  kfusion.setKinectDeviceDepth(depthImage.getDeviceImage());
  Stats.sample("raw to cooked");

  integrate = kfusion.Track();
  Stats.sample("track");

  if(integrate || reset){
    kfusion.Integrate();
    Stats.sample("integrate");
    reset = false;
  }

  translation = kfusion.pose.get_translation();


//  if (translation.x<=0.75 || translation.x>=1.25 ||
//      translation.y<=0.75 || translation.y>=1.25 ||
//      translation.z<=-0.25 || translation.z>=0.25)
//  {
//    kfusion.Reset();
//    kfusion.setPose(toMatrix4(initPose));
//    reset = true;
//  }
  renderLight( lightModel.getDeviceImage(), kfusion.vertex, kfusion.normal, translation, ambient);
  renderLight( lightScene.getDeviceImage(), kfusion.inputVertex[0], kfusion.inputNormal[0], translation, ambient );
  renderTrackResult( depth.getDeviceImage(), kfusion.reduction );
  cudaDeviceSynchronize();

  Stats.sample("render");

  glClear(GL_COLOR_BUFFER_BIT);
  glRasterPos2i(0,imageSize.y * 0);
  glDrawPixels(lightScene);
  glRasterPos2i(imageSize.x, imageSize.y * 0);
  glDrawPixels(depth);
  glRasterPos2i(0,imageSize.y * 1);
  glDrawPixels(lightModel);
  const double endProcessing = Stats.sample("draw");

  Stats.sample("total", endProcessing - startFrame, PerfStats::TIME);
  Stats.sample("total_proc", endProcessing - startProcessing, PerfStats::TIME);

  if(printCUDAError())
    exit(1);

  ++counter;

  if(counter % 50 == 0){
    Stats.print();
    Stats.reset();
    cout << endl;
  }

  glutSwapBuffers();
}

void idle(void){
    glutPostRedisplay();
}

void keys(unsigned char key, int x, int y){
    switch(key){
    case 'c':
        kfusion.Reset();
        kfusion.setPose(toMatrix4(initPose));
        reset = true;
        break;
    case 'q':
        exit(0);
        break;
    }
}

void reshape(int width, int height){
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glViewport(0, 0, width, height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glColor3f(1.0f,1.0f,1.0f);
    glRasterPos2f(-1, 1);
    glOrtho(-0.375, width-0.375, height-0.375, -0.375, -1 , 1); //offsets to make (0,0) the top left pixel (rather than off the display)
    glPixelZoom(1,-1);
}

void exitFunc(void){
    CloseKinect();
    kfusion.Clear();
    cudaDeviceReset();
}

int main(int argc, char ** argv) {
    const float size = (argc > 1) ? atof(argv[1]) : 2.f;

    KFusionConfig config;

    // it is enough now to set the volume resolution once.
    // everything else is derived from that.
    // config.volumeSize = make_uint3(64);
    config.volumeSize = make_uint3(128);
    // config.volumeSize = make_uint3(256);

    // these are physical dimensions in meters
    config.volumeDimensions = make_float3(size);
    config.nearPlane = 0.4f;
    config.farPlane = 5.0f;
    config.mu = 0.1;
    config.combinedTrackAndReduce = false;

  // change the following parameters for using 640 x 480 input images
  //    config.inputSize = make_uint2(320,240);
  //    config.camera =  make_float4(297.12732, 296.24240, 169.89365, 121.25151);
  config.inputSize = make_uint2(640,480);
  config.camera =  make_float4(297.12732*2., 296.24240*2.,
                               169.89365*2., 121.25151*2.);

    // config.iterations is a vector<int>, the length determines
    // the number of levels to be used in tracking
    // push back more then 3 iteraton numbers to get more levels.
    config.iterations[0] = 10;
    config.iterations[1] = 5;
    config.iterations[2] = 4;

    config.dist_threshold = (argc > 2 ) ? atof(argv[2]) : config.dist_threshold;
    config.normal_threshold = (argc > 3 ) ? atof(argv[3]) : config.normal_threshold;

    initPose = SE3<float>(makeVector(size/2, size/2, 0, 0, 0, 0));

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGB | GLUT_DOUBLE );
    glutInitWindowSize(config.inputSize.x * 2, config.inputSize.y * 2);
    glutCreateWindow("kfusion");

    kfusion.Init(config);
    if(printCUDAError())
        exit(1);

    kfusion.setPose(toMatrix4(initPose));

    lightScene.alloc(config.inputSize), depth.alloc(config.inputSize), lightModel.alloc(config.inputSize);
    depthImage.alloc(make_uint2(640, 480));

    if(InitKinect(depthImage.data()))
        exit(1);

    atexit(exitFunc);
    glutDisplayFunc(display);
    glutKeyboardFunc(keys);
    glutReshapeFunc(reshape);
    glutIdleFunc(idle);

    glutMainLoop();

    return 0;
}
