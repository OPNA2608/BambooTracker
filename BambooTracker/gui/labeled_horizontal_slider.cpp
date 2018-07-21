#include "labeled_horizontal_slider.hpp"
#include "ui_labeled_horizontal_slider.h"
#include "slider_style.hpp"

LabeledHorizontalSlider::LabeledHorizontalSlider(QWidget *parent) :
	QFrame(parent),
	ui(new Ui::LabeledHorizontalSlider)
{
	init("");
}

LabeledHorizontalSlider::LabeledHorizontalSlider(QString text, QWidget *parent) :
	QFrame(parent),
	ui(new Ui::LabeledHorizontalSlider)
{
	init(text);
}

void LabeledHorizontalSlider::init(QString text)
{
	ui->setupUi(this);
	ui->textLabel->setText(text);
	ui->valueLabel->setText(QString::number(ui->slider->value()));
	ui->slider->setStyle(new SliderStyle());
}

LabeledHorizontalSlider::~LabeledHorizontalSlider()
{
	delete ui;
}

int LabeledHorizontalSlider::value() const
{
	return ui->slider->value();
}

void LabeledHorizontalSlider::setValue(int value)
{
	ui->slider->setValue(value);
}

int LabeledHorizontalSlider::maximum() const
{
	return ui->slider->maximum();
}

void LabeledHorizontalSlider::setMaximum(int value)
{
	ui->slider->setMaximum(value);
}

int LabeledHorizontalSlider::minimum() const
{
	return ui->slider->minimum();
}

void LabeledHorizontalSlider::setMinimum(int value)
{
	ui->slider->setMinimum(value);
}

QString LabeledHorizontalSlider::text() const
{
	return ui->textLabel->text();
}

void LabeledHorizontalSlider::setText(QString text)
{
	ui->textLabel->setText(text);
}

void LabeledHorizontalSlider::on_slider_valueChanged(int value)
{
	ui->valueLabel->setText(QString::number(value));
	emit valueChanged(value);
}
